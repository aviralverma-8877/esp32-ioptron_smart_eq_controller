// xlate.cpp - LX200 / NexStar client protocol -> iOptron 2014 command language.
//
// LX200 (Meade): ':'..'#' framed ASCII. Stellarium prefixes every command with
// '#' (flush), so bytes outside a frame are ignored; ACK (0x06) is the one
// unframed query. Coordinates are JNow; replies use '*' as the degree mark
// (like OnStep - Stellarium, SkySafari, ASCOM and INDI all accept it).
// NexStar (Celestron): one command letter + fixed-length arguments, replies end
// in '#'. Positions are fractions of a turn as 16-bit ("E"/"R") or 32-bit
// ("e"/"r") hex.
//
// Mount side, all as used by iOptron Commander on the SmartEQ Pro:
//   :GEC#  position          :GAS#  status (slewing / tracking / hemisphere)
//   :ST1#  tracking on       :Sr :Sd target, then :MS# slew ('1' ok / '0' below horizon)
//   :CM#   sync to target    :q#    stop      :MH# zero position
//   :SRn#  move rate 1..9    :mn# :ms# :me# :mw# move   :qR# :qD# stop one axis
//   :RT0..2# sidereal / lunar / solar tracking
// Time and site setters from the client are acknowledged but NOT written to the
// mount - the GPS sync owns those (a wrong :SL/:Sg breaks every GoTo).
#include "xlate.h"
#include "timeutil.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XLOG(...) do { if (io.log) io.log(__VA_ARGS__); } while (0)

static XlateIo   io = {};
static XlateMode mode = XL_NATIVE;

static const uint32_t FRAME_MS   = 1000;    // partial command older than this is dropped
static const uint32_t POS_MS     = 250;     // :GEC# reply reused for :GR# + :GD# pairs
static const uint32_t STATUS_MS  = 300;
static const uint32_t SLEW_MAX_MS = 240000;

static void out(const char *s)                 { io.write((const uint8_t *)s, strlen(s)); }
static void outb(const uint8_t *b, size_t n)   { io.write(b, n); }

static bool allDigits(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
  return true;
}
static long num(const char *s, int len) {
  char b[12]; memcpy(b, s, len); b[len] = 0; return atol(b);
}

// ---- mount side ---------------------------------------------------------------
static bool ask1(const char *cmd) {               // '1' = OK, no '#'
  char r[4] = "";
  bool ok = io.ask(cmd, r, sizeof r, 700, 1) == 1 && r[0] == '1';
  XLOG("[xl] %s -> %s\n", cmd, ok ? "1" : (r[0] ? r : "(none)"));
  return ok;
}
static void sendOnly(const char *cmd) {           // :m?# has no reply
  char r[4];
  io.ask(cmd, r, sizeof r, 0, 1);
  XLOG("[xl] %s\n", cmd);
}

static double   posRa = 0, posDec = 0;
static bool     posOk = false;
static uint32_t posMs = 0;

static bool readPos(double &ra, double &dec) {
  uint32_t now = io.millis();
  if (!posOk || now - posMs > POS_MS) {
    char r[40];
    size_t n = io.ask(":GEC#", r, sizeof r, 500, 0);
    if (n >= 17 && (r[0] == '+' || r[0] == '-') && allDigits(r + 1, 16)) {
      posDec = num(r + 1, 8) / 360000.0 * (r[0] == '-' ? -1 : 1);   // 0.01"
      posRa  = num(r + 9, 8) / 3600000.0;                           // ms
      posOk = true; posMs = now;
    } else if (now - posMs > 5000) posOk = false;                   // keep a short gap
  }
  ra = posRa; dec = posDec;
  return posOk;
}

static int      stCode = -1;                       // :GAS# digit 2, -1 unknown
static bool     stNorth = true;
static uint32_t stMs = 0;
static bool     stFresh = false;

static int readStatus() {
  uint32_t now = io.millis();
  if (!stFresh || now - stMs > STATUS_MS) {
    char r[16];
    size_t n = io.ask(":GAS#", r, sizeof r, 500, 0);
    if (n >= 6 && allDigits(r, 6)) { stCode = r[1] - '0'; stNorth = r[5] != '0'; }
    else stCode = -1;
    stMs = now; stFresh = true;
  }
  return stCode;
}
static bool isTracking(int st) { return st == 1 || st == 3 || st == 5; }   // track / guide / PEC

// After a GoTo: once the slew ends, make sure the mount is tracking (it can end
// "stopped", e.g. after a zero-position slew) - same as the controller's TRACK.
static bool     watch = false, watchSeen = false;
static uint32_t watchMs = 0, watchPoll = 0;

static bool setTarget(double raH, double decD) {
  raH = fmod(raH, 24.0); if (raH < 0) raH += 24.0;
  long raMs = lround(raH * 3600000.0); if (raMs >= 86400000L) raMs -= 86400000L;
  long dec01 = lround(decD * 360000.0);
  char c[32];
  snprintf(c, sizeof c, ":Sr%08ld#", raMs);
  if (!ask1(c)) return false;
  snprintf(c, sizeof c, ":Sd%c%08ld#", dec01 < 0 ? '-' : '+', labs(dec01));
  return ask1(c);
}

enum GotoRes { G_OK, G_BELOW, G_ERR };
static GotoRes gotoRaDec(double raH, double decD) {
  XLOG("[xl] goto RA %.5fh Dec %+.5f\n", raH, decD);
  if (!ask1(":ST1#") || !setTarget(raH, decD)) return G_ERR;
  char r[4] = "";
  size_t n = io.ask(":MS#", r, sizeof r, 700, 1);
  XLOG("[xl] :MS# -> %s\n", n ? r : "(none)");
  posOk = false; stFresh = false;
  if (n && r[0] == '1') { watch = true; watchSeen = false; watchMs = watchPoll = io.millis(); return G_OK; }
  return (n && r[0] == '0') ? G_BELOW : G_ERR;
}

static bool syncRaDec(double raH, double decD) {
  XLOG("[xl] sync RA %.5fh Dec %+.5f\n", raH, decD);
  bool ok = setTarget(raH, decD) && ask1(":CM#");
  posOk = false;
  return ok;
}

static void stopAll() { ask1(":q#"); watch = false; stFresh = false; }

static void setRate(int n) {                       // iOptron 1..9 = 1x 2x 8x 16x 64x 128x 256x 512x max
  char c[8];
  snprintf(c, sizeof c, ":SR%d#", n < 1 ? 1 : n > 9 ? 9 : n);
  ask1(c);
}
static void moveAxis(char dir, bool on) {          // dir n/s/e/w
  if (!on) { ask1(dir == 'n' || dir == 's' ? ":qD#" : ":qR#"); return; }
  char c[8];
  snprintf(c, sizeof c, ":m%c#", dir);
  sendOnly(c);
}

// ---- shared formatting ----------------------------------------------------------
static void localNow(bool &ok, int &Y, int &Mo, int &D, int &h, int &mi, int &s, long &off) {
  long utc = 0; off = 0;
  ok = io.clock && io.clock(utc, off);
  if (!ok) { Y = 2000; Mo = D = 1; h = mi = s = 0; return; }
  long loc = utc + off * 60;
  long days = loc / 86400, tod = loc % 86400;
  if (tod < 0) { tod += 86400; days--; }
  civilFromDays(days, Y, Mo, D);
  h = tod / 3600; mi = tod / 60 % 60; s = tod % 60;
}

// ================================ LX200 ===========================================
static char     lxBuf[48];
static uint8_t  lxN = 0;
static bool     lxIn = false, lxLong = true;       // high precision until :U# toggles it
static uint32_t lxMs = 0;
static double   tgtRa = 0, tgtDec = 0;
static bool     tgtRaOk = false, tgtDecOk = false;

// "HH:MM:SS#" or "HH:MM.T#"
static void lxHms(double h, char *b, size_t n) {
  if (lxLong) {
    long s = lround(fmod(h, 24.0) * 3600.0); s = ((s % 86400) + 86400) % 86400;
    snprintf(b, n, "%02ld:%02ld:%02ld#", s / 3600, s / 60 % 60, s % 60);
  } else {
    long t = lround(fmod(h, 24.0) * 600.0); t = ((t % 14400) + 14400) % 14400;
    snprintf(b, n, "%02ld:%02ld.%ld#", t / 600, t / 10 % 60, t % 10);
  }
}
// "sDD*MM:SS#" / "sDD*MM#" (signed) or "DDD*MM:SS#" / "DDD*MM#" (azimuth)
static void lxDms(double d, bool az, bool lng, char *b, size_t n) {
  char sg = d < 0 ? '-' : '+';
  if (az) { d = fmod(d, 360.0); if (d < 0) d += 360.0; }
  double a = fabs(d);
  if (lng) {
    long s = lround(a * 3600.0); if (az) s %= 360L * 3600;
    if (az) snprintf(b, n, "%03ld*%02ld:%02ld#", s / 3600, s / 60 % 60, s % 60);
    else    snprintf(b, n, "%c%02ld*%02ld:%02ld#", sg, s / 3600, s / 60 % 60, s % 60);
  } else {
    long m = lround(a * 60.0); if (az) m %= 360L * 60;
    if (az) snprintf(b, n, "%03ld*%02ld#", m / 60, m % 60);
    else    snprintf(b, n, "%c%02ld*%02ld#", sg, m / 60, m % 60);
  }
}

static bool isSep(char c) {
  return c == ':' || c == '*' || c == '\'' || c == ' ' || (uint8_t)c == 0xDF;
}
// "HH:MM:SS", "HH:MM.T", "HH:MM:SS.S", optional leading space
static bool lxParseHms(const char *s, double &h) {
  char *e;
  while (*s == ' ') s++;
  long hh = strtol(s, &e, 10);
  if (e == s || *e != ':') return false;
  s = e + 1;
  double mm = strtod(s, &e), ss = 0;
  if (e == s) return false;
  if (*e == ':') { s = e + 1; ss = strtod(s, &e); if (e == s) return false; }
  if (hh < 0 || hh > 23 || mm < 0 || mm >= 60 || ss < 0 || ss >= 60) return false;
  h = hh + mm / 60.0 + ss / 3600.0;
  return true;
}
// "sDD*MM:SS", "sDD*MM", separators * : ' 0xDF
static bool lxParseDms(const char *s, double &d) {
  char *e;
  while (*s == ' ') s++;
  int sg = 1;
  if (*s == '-') { sg = -1; s++; } else if (*s == '+') s++;
  long dd = strtol(s, &e, 10);
  if (e == s || !isSep(*e)) return false;
  s = e + 1;
  double mm = strtod(s, &e), ss = 0;
  if (e == s) return false;
  if (isSep(*e) && e[1]) { s = e + 1; ss = strtod(s, &e); if (e == s) return false; }
  if (dd < 0 || mm < 0 || mm >= 60 || ss < 0 || ss >= 60) return false;
  double v = dd + mm / 60.0 + ss / 3600.0;
  if (v > 90.0) return false;
  d = sg * v;
  return true;
}

static void lxCommand(const char *c) {
  char b[64];
  double ra, dec;
  if (c[0] == 'G') {                               // ---- getters
    const char *q = c + 1;
    if (!strcmp(q, "R")) { if (readPos(ra, dec)) { lxHms(ra, b, sizeof b); out(b); } return; }
    if (!strcmp(q, "D")) { if (readPos(ra, dec)) { lxDms(dec, false, lxLong, b, sizeof b); out(b); } return; }
    if (!strcmp(q, "A") || !strcmp(q, "Z")) {
      double alt, az;
      if (!readPos(ra, dec) || !io.altAz || !io.altAz(ra, dec, alt, az)) return;
      lxDms(q[0] == 'A' ? alt : az, q[0] == 'Z', lxLong, b, sizeof b); out(b);
      return;
    }
    if (!strcmp(q, "VP")) { out("SmartEQ-RJ9#"); return; }
    if (!strcmp(q, "VN")) { out("1.0#"); return; }
    if (!strcmp(q, "VD")) { snprintf(b, sizeof b, "%s#", __DATE__); out(b); return; }
    if (!strcmp(q, "VT")) { snprintf(b, sizeof b, "%s#", __TIME__); out(b); return; }
    if (!strcmp(q, "c"))  { out("24#"); return; }
    if (!strcmp(q, "T"))  { out("60.1#"); return; }  // sidereal tracking frequency
    if (!strcmp(q, "L") || !strcmp(q, "a") || !strcmp(q, "C") || !strcmp(q, "G")) {
      bool ok; int Y, Mo, D, h, mi, s; long off;
      localNow(ok, Y, Mo, D, h, mi, s, off);
      if (q[0] == 'L') snprintf(b, sizeof b, "%02d:%02d:%02d#", h, mi, s);
      else if (q[0] == 'a') snprintf(b, sizeof b, "%02d:%02d:%02d#", h % 12 ? h % 12 : 12, mi, s);
      else if (q[0] == 'C') snprintf(b, sizeof b, "%02d/%02d/%02d#", Mo, D, Y % 100);
      else {                                       // hours to ADD to local time for UTC
        long u = -off;
        if (u % 60) snprintf(b, sizeof b, "%c%02ld.%ld#", u < 0 ? '-' : '+', labs(u) / 60, labs(u) % 60 / 6);
        else        snprintf(b, sizeof b, "%c%02ld#", u < 0 ? '-' : '+', labs(u) / 60);
      }
      out(b);
      return;
    }
    if (!strcmp(q, "t") || !strcmp(q, "g")) {
      double lat = 0, lon = 0;
      if (io.site) io.site(lat, lon);
      if (q[0] == 't') { lxDms(lat, false, false, b, sizeof b); out(b); return; }
      double w = fmod(-lon, 360.0); if (w < 0) w += 360.0;       // LX200: west-positive 0..360
      long m = lround(w * 60.0) % (360 * 60);
      snprintf(b, sizeof b, "%03ld*%02ld#", m / 60, m % 60); out(b);
      return;
    }
    if (!strcmp(q, "W")) {                         // Autostar II: mount type, tracking, aligned
      int st = readStatus();
      snprintf(b, sizeof b, "G%c1#", isTracking(st) ? 'T' : 'N'); out(b);
      return;
    }
  } else if (c[0] == 'S') {                        // ---- setters
    double v;                                      // invalid input keeps the old target
    if (c[1] == 'r') { bool ok = lxParseHms(c + 2, v); if (ok) { tgtRa = v; tgtRaOk = true; } out(ok ? "1" : "0"); return; }
    if (c[1] == 'd') { bool ok = lxParseDms(c + 2, v); if (ok) { tgtDec = v; tgtDecOk = true; } out(ok ? "1" : "0"); return; }
    if (c[1] == 'C') {                             // date: Meade replies with two update strings
      XLOG("[xl] ignored (GPS owns time): :%s#\n", c);
      out("1Updating Planetary Data#                              #");
      return;
    }
    if (c[1]) { XLOG("[xl] ignored (GPS owns time/site): :%s#\n", c); out("1"); return; }
  } else if (!strcmp(c, "MS")) {                   // ---- motion
    if (!tgtRaOk || !tgtDecOk) { out("2No target set#"); return; }
    switch (gotoRaDec(tgtRa, tgtDec)) {
      case G_OK:    out("0"); break;
      case G_BELOW: out("1Object below horizon#"); break;
      default:      out("2Mount did not answer#"); break;
    }
    return;
  } else if (!strcmp(c, "CM") || !strcmp(c, "CMR")) {
    if (tgtRaOk && tgtDecOk && !syncRaDec(tgtRa, tgtDec)) XLOG("[xl] sync FAILED\n");
    out("Coordinates     matched.        #");      // Meade reply; no error form exists
    return;
  } else if (c[0] == 'M' && c[1] && !c[2] && strchr("nsew", c[1])) { moveAxis(c[1], true); return; }
  else if (!strcmp(c, "Q")) { stopAll(); return; }
  else if (c[0] == 'Q' && c[1] && !c[2] && strchr("nsew", c[1])) { moveAxis(c[1], false); return; }
  else if (c[0] == 'R' && c[1] && c[2] == 0) {     // move rate
    switch (c[1]) {
      case 'G': setRate(2); return;                // guide   2x
      case 'C': setRate(4); return;                // centre 16x
      case 'M': setRate(6); return;                // find  128x
      case 'S': setRate(9); return;                // slew   max
      default:
        if (c[1] >= '0' && c[1] <= '9') { setRate(c[1] == '0' ? 1 : c[1] - '0'); return; }
    }
  } else if (!strcmp(c, "U")) { lxLong = !lxLong; return; }
  else if (!strcmp(c, "P")) { lxLong = !lxLong; out(lxLong ? "HIGH PRECISION" : "LOW PRECISION"); return; }
  else if (!strcmp(c, "D")) { out(readStatus() == 2 ? "\x7f#" : "#"); return; }
  else if (!strcmp(c, "Te")) { out(ask1(":ST1#") ? "1" : "0"); return; }
  else if (!strcmp(c, "Td")) { out(ask1(":ST0#") ? "1" : "0"); return; }
  else if (!strcmp(c, "TQ")) { ask1(":RT0#"); return; }
  else if (!strcmp(c, "TL")) { ask1(":RT1#"); return; }
  else if (!strcmp(c, "TS")) { ask1(":RT2#"); return; }
  else if (!strcmp(c, "hP")) { ask1(":MH#"); return; }      // park -> zero position
  else if (!strcmp(c, "AP") || !strcmp(c, "AL") || !strcmp(c, "AA")) return;   // alignment mode: no-op
  XLOG("[xl] LX200 unsupported: :%s#\n", c);
}

static void lxFeed(uint8_t c) {
  if (!lxIn) {
    if (c == 0x06) { out("P"); return; }         // ACK = alignment query -> polar
    if (c == ':') { lxIn = true; lxN = 0; lxMs = io.millis(); }
    return;                                        // '#' flush, CR/LF, noise
  }
  if (c == '#') { lxBuf[lxN] = 0; lxIn = false; lxCommand(lxBuf); return; }
  if (lxN < sizeof lxBuf - 1) lxBuf[lxN++] = (char)c;
  else lxIn = false;                               // overlong: drop it
}

// =============================== NexStar ==========================================
static uint8_t  nxBuf[20];
static uint8_t  nxN = 0, nxNeed = 0;
static uint32_t nxMs = 0;

static int nxArgLen(uint8_t c) {                   // argument bytes after the command letter
  switch (c) {
    case 'E': case 'e': case 'Z': case 'z': case 'L': case 'M': case 'J':
    case 'V': case 'm': case 't': case 'h': case 'w': return 0;
    case 'K': case 'T': return 1;
    case 'P': return 7;
    case 'H': case 'W': return 8;
    case 'R': case 'S': case 'B': return 9;        // "XXXX,XXXX"
    case 'r': case 's': case 'b': return 17;       // "XXXXXXXX,XXXXXXXX"
    default: return -1;
  }
}

static uint32_t turn32(double frac) {              // fraction of a turn -> 32-bit
  frac -= floor(frac);
  return (uint32_t)((uint64_t)llround(frac * 4294967296.0) & 0xFFFFFFFFu);
}
static double degTurn(double d) { return (d < 0 ? d + 360.0 : d) / 360.0; }

static void nxPair(char cmd, double a, double b) { // a, b as fractions of a turn
  char s[24];
  uint32_t x = turn32(a), y = turn32(b);
  if (cmd >= 'a') snprintf(s, sizeof s, "%08lX,%08lX#", (unsigned long)(x & 0xFFFFFF00u),
                           (unsigned long)(y & 0xFFFFFF00u));
  else snprintf(s, sizeof s, "%04X,%04X#", (unsigned)((((uint64_t)x + 0x8000) >> 16) & 0xFFFF),
                (unsigned)((((uint64_t)y + 0x8000) >> 16) & 0xFFFF));
  out(s);
}

static bool hexVal(const uint8_t *s, int w, uint32_t &v) {
  v = 0;
  for (int i = 0; i < w; i++) {
    uint8_t c = s[i]; int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else return false;
    v = (v << 4) | d;
  }
  return true;
}
// "XXXX,XXXX" / "XXXXXXXX,XXXXXXXX" -> RA hours, Dec degrees
static bool nxParsePair(const uint8_t *s, int w, double &raH, double &decD) {
  uint32_t a, b;
  if (s[w] != ',' || !hexVal(s, w, a) || !hexVal(s + w + 1, w, b)) return false;
  double full = w == 4 ? 65536.0 : 4294967296.0;
  raH = a / full * 24.0;
  decD = b / full * 360.0;
  if (decD > 180.0) decD -= 360.0;
  return decD >= -90.0 && decD <= 90.0;
}

// iOptron move rate closest (in log) to `x` times sidereal
static int rateFor(double x) {
  static const double R[9] = { 1, 2, 8, 16, 64, 128, 256, 512, 1000 };
  int best = 0;
  for (int i = 1; i < 9; i++) if (fabs(log(x / R[i])) < fabs(log(x / R[best]))) best = i;
  return best + 1;
}

// 'P' n dest id d1 d2 d3 nResp - hand-controller passthrough; only the axis
// moves are acted on. dest 16 = RA (azm) motor, 17 = Dec (alt) motor.
static void nxPassthrough(const uint8_t *a) {
  uint8_t dest = a[1], id = a[2], nResp = a[6];
  uint8_t resp[8] = {};
  if (dest == 16 || dest == 17) {
    bool ra = dest == 16;
    if (id == 36 || id == 37 || id == 6 || id == 7) {
      bool pos = id == 36 || id == 6;
      char dir = ra ? (pos ? 'e' : 'w') : (pos ? 'n' : 's');
      int rate = 0;
      if (id >= 36) rate = a[3];                   // fixed rate 0..9
      else {                                       // variable, 0.25"/s
        unsigned v = (a[3] << 8) | a[4];
        if (v) rate = rateFor(v / 4.0 / 15.041);
      }
      if (rate <= 0) moveAxis(dir, false);
      else { setRate(rate > 9 ? 9 : rate); moveAxis(dir, true); }
    } else if (id == 254) { resp[0] = 7; resp[1] = 11; }   // motor firmware version
  } else XLOG("[xl] NexStar passthrough ignored: dest %u id %u\n", dest, id);
  if (nResp > sizeof resp) nResp = sizeof resp;
  outb(resp, nResp);
  out("#");
}

static void nxCommand(const uint8_t *c) {
  double ra, dec;
  uint8_t b[10];
  switch (c[0]) {
    case 'E': case 'e':
      if (readPos(ra, dec)) nxPair(c[0], ra / 24.0, degTurn(dec));
      return;
    case 'Z': case 'z': {
      double alt, az;
      if (readPos(ra, dec) && io.altAz && io.altAz(ra, dec, alt, az)) nxPair(c[0], az / 360.0, degTurn(alt));
      return;
    }
    case 'R': case 'r': case 'S': case 's': {
      int w = (c[0] == 'r' || c[0] == 's') ? 8 : 4;
      if (!nxParsePair(c + 1, w, ra, dec)) XLOG("[xl] NexStar bad coordinates\n");
      else if (c[0] == 'R' || c[0] == 'r') {
        if (gotoRaDec(ra, dec) != G_OK) XLOG("[xl] GoTo rejected\n");
      } else if (!syncRaDec(ra, dec)) XLOG("[xl] sync FAILED\n");
      out("#");
      return;
    }
    case 'B': case 'b': XLOG("[xl] NexStar alt-az GoTo unsupported\n"); out("#"); return;
    case 'L': out(readStatus() == 2 ? "1#" : "0#"); return;
    case 'M': stopAll(); out("#"); return;
    case 'J': out("1#"); return;                   // always "aligned"
    case 'K': b[0] = c[1]; b[1] = '#'; outb(b, 2); return;
    case 'V': b[0] = 4; b[1] = 21; b[2] = '#'; outb(b, 3); return;    // HC firmware 4.21
    case 'm': b[0] = 20; b[1] = '#'; outb(b, 2); return;              // model 20 = AVX (GEM)
    case 't': {
      int st = readStatus();
      b[0] = isTracking(st) ? (stNorth ? 2 : 3) : 0;                  // 2 EQ north, 3 EQ south
      b[1] = '#'; outb(b, 2);
      return;
    }
    case 'T': ask1(c[1] ? ":ST1#" : ":ST0#"); out("#"); return;
    case 'P': nxPassthrough(c + 1); return;
    case 'h': {                                    // Q R S T U V W X: local time, UTC offset (h), DST
      bool ok; int Y, Mo, D, h, mi, s; long off;
      localNow(ok, Y, Mo, D, h, mi, s, off);
      b[0] = h; b[1] = mi; b[2] = s; b[3] = Mo; b[4] = D; b[5] = (uint8_t)(Y - 2000);
      b[6] = (uint8_t)(int8_t)lround(off / 60.0); b[7] = 0; b[8] = '#';
      outb(b, 9);
      return;
    }
    case 'w': {                                    // lat d m s N/S, lon d m s E/W
      double lat = 0, lon = 0;
      if (io.site) io.site(lat, lon);
      long la = lround(fabs(lat) * 3600.0), lo = lround(fabs(lon) * 3600.0);
      b[0] = la / 3600; b[1] = la / 60 % 60; b[2] = la % 60; b[3] = lat < 0;
      b[4] = lo / 3600; b[5] = lo / 60 % 60; b[6] = lo % 60; b[7] = lon < 0;
      b[8] = '#';
      outb(b, 9);
      return;
    }
    case 'H': case 'W': XLOG("[xl] ignored (GPS owns time/site): NexStar %c\n", c[0]); out("#"); return;
  }
}

static void nxFeed(uint8_t c) {
  if (nxN == 0) {
    int n = nxArgLen(c);
    if (n < 0) return;                             // not a command letter: skip
    nxNeed = (uint8_t)(n + 1);
    nxMs = io.millis();
  }
  nxBuf[nxN++] = c;
  if (nxN >= nxNeed) { nxN = 0; nxCommand(nxBuf); }
}

// ================================ public ==========================================
void xlateBegin(const XlateIo &i) { io = i; xlateReset(); }

void xlateSetMode(XlateMode m) {
  if (m >= XL_COUNT) m = XL_NATIVE;
  mode = m;
  xlateReset();
}

XlateMode xlateMode() { return mode; }

const char *xlateModeName(XlateMode m) {
  switch (m) {
    case XL_LX200:   return "LX200";
    case XL_NEXSTAR: return "NexStar";
    default:         return "iOptron";
  }
}

void xlateReset() {
  lxIn = false; lxN = 0; lxLong = true;
  nxN = 0;
  tgtRaOk = tgtDecOk = false;
  posOk = false; stFresh = false;
  watch = false;
}

void xlateFeed(uint8_t c) {
  if (mode == XL_LX200) lxFeed(c);
  else if (mode == XL_NEXSTAR) nxFeed(c);
}

void xlateTick() {
  if (!io.millis) return;
  uint32_t now = io.millis();
  if (lxIn && now - lxMs > FRAME_MS) lxIn = false;
  if (nxN && now - nxMs > FRAME_MS) nxN = 0;
  if (watch && now - watchPoll >= 1000) {
    watchPoll = now;
    int st = readStatus();
    if (st == 2) watchSeen = true;
    else if (watchSeen || now - watchMs > 3000) {
      if (st == 0) ask1(":ST1#");                  // slew ended stopped: resume tracking
      watch = false;
    }
    if (now - watchMs > SLEW_MAX_MS) watch = false;
  }
}
