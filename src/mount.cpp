#include "mount.h"
#include "config.h"
#include "bt_bridge.h"     // btConnected (link timeout depends on it)
#include <string.h>

static HardwareSerial   Rs232(2);
static const size_t     CHUNK          = 256;
static const size_t     UART_RX_BUFFER = 2048;

MountState M;
bool       linkUp = false;
static uint32_t lastMountRxMs = 0;      // last time the mount sent us anything

// ---- link tracking --------------------------------------------------------
bool mountLinkUp() {
  uint32_t to = btConnected ? 8000 : 2500;
  return lastMountRxMs && (millis() - lastMountRxMs < to);
}
void mountUpdateLink() { linkUp = mountLinkUp(); }

// ---- raw I/O ------------------------------------------------------------
void mountDrainRx() { while (Rs232.available()) Rs232.read(); }

static void pump(Stream &in, Print &out, bool fromMount) {
  uint8_t buf[CHUNK];
  int avail;
  while ((avail = in.available()) > 0) {
    size_t want = avail > (int)CHUNK ? CHUNK : (size_t)avail;
    size_t got  = in.readBytes(buf, want);
    if (got) {
      out.write(buf, got);
      if (fromMount) lastMountRxMs = millis();
    }
  }
}
void pumpClientToMount(Stream &client) { pump(client, Rs232, false); }
void pumpMountToClient(Print  &client) { pump(Rs232, client, true); }

size_t askMount(const char *cmd, char *out, size_t outSz,
                uint16_t timeoutMs, uint8_t fixedLen) {
  mountDrainRx();
  Rs232.write((const uint8_t *)cmd, strlen(cmd));
  size_t n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs && n < outSz - 1) {
    if (Rs232.available()) {
      char c = (char)Rs232.read();
      out[n++] = c;
      if (fixedLen == 0 && c == '#') break;
      if (fixedLen && n >= fixedLen) break;
      t0 = millis();
    }
  }
  out[n] = 0;
  if (n > 0) lastMountRxMs = millis();
  return n;
}

// ---- small parse helpers ---------------------------------------------
static bool allDigits(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
  return n > 0;
}
static long sub2l(const char *s, int off, int len) {
  char b[12]; if (len > 11) len = 11;
  memcpy(b, s + off, len); b[len] = 0; return atol(b);
}

// ---- iOptron reply decoders ----------------------------------------
static const char *modelName(const char *code) {
  struct { const char *c, *n; } t[] = {
    {"0010","SmartEQ"},     {"0011","SmartEQ Pro"}, {"0025","CEM25"},
    {"0026","CEM26"},       {"0027","CEM26-EC"},    {"0028","GEM28"},
    {"0030","iEQ30 Pro"},   {"0040","CEM40"},       {"0041","CEM40-EC"},
    {"0043","GEM45"},       {"0044","GEM45-EC"},    {"0045","iEQ45 Pro"},
    {"0046","iEQ45 Pro AA"},{"0060","CEM60"},       {"0061","CEM60-EC"},
    {"0070","CEM70"},       {"0071","CEM70-EC"},    {"5010","Cube II EQ"},
    {"5035","AZ Mount Pro"},
  };
  for (auto &e : t) if (!strcmp(e.c, code)) return e.n;
  return "mount";
}

static void decGLT(const char *r) {                // sMMMd YYMMDD HHMMSS #
  if (strlen(r) < 17 || (r[0] != '+' && r[0] != '-')) return;
  M.offsetMin = sub2l(r, 1, 3) * (r[0] == '-' ? -1 : 1);
  M.dstFlag   = r[4] - '0';
  long ymd = sub2l(r, 5, 6), hms = sub2l(r, 11, 6);
  int Y = 2000 + ymd / 10000, Mo = (ymd / 100) % 100, D = ymd % 100;
  int h = hms / 10000, mi = (hms / 100) % 100, s = hms % 100;
  snprintf(M.utcOffset, sizeof M.utcOffset, "%c%02ld:%02ld",
           M.offsetMin < 0 ? '-' : '+', labs(M.offsetMin) / 60, labs(M.offsetMin) % 60);
  snprintf(M.dst, sizeof M.dst, "%s", M.dstFlag ? "on" : "off");
  snprintf(M.dateStr, sizeof M.dateStr, "%04d-%02d-%02d", Y, Mo, D);
  snprintf(M.timeStr, sizeof M.timeStr, "%02d:%02d:%02d", h, mi, s);
}
static void decGG(const char *r) {                 // longitude, 0.01"  (:Gg#)
  if (strlen(r) < 5 || (r[0] != '+' && r[0] != '-')) return;
  double d = atol(r + 1) / 360000.0 * (r[0] == '-' ? -1 : 1);
  snprintf(M.lonStr, sizeof M.lonStr, "%.4f%c", fabs(d), d < 0 ? 'W' : 'E');
}
static void decGT(const char *r) {                 // latitude, 0.01"  (:Gt#)
  if (strlen(r) < 5 || (r[0] != '+' && r[0] != '-')) return;
  double d = atol(r + 1) / 360000.0 * (r[0] == '-' ? -1 : 1);
  snprintf(M.latStr, sizeof M.latStr, "%.4f%c%s", fabs(d), d < 0 ? 'S' : 'N',
           fabs(d) > 90.0 ? "?" : "");
}
static void decGAS(const char *r) {                // G S T M X H
  if (strlen(r) < 6 || !allDigits(r, 6)) return;
  int st = r[1] - '0', tr = r[2] - '0', sp = r[3] - '0', hm = r[5] - '0';
  const char *S[] = {"Stopped","Tracking","Slewing","Guiding","Mrdn flip",
                     "Track+PEC","Parked","At home"};
  const char *T[] = {"Sidereal","Lunar","Solar","King","Custom"};
  const char *R[] = {"-","1x","2x","8x","16x","64x","128x","256x","512x","MAX"};
  const char *X[] = {"RS232","hand ctl","GPS"};
  snprintf(M.statusStr, sizeof M.statusStr, "%s", st >= 0 && st < 8 ? S[st] : "?");
  snprintf(M.trackStr,  sizeof M.trackStr,  "%s", tr >= 0 && tr < 5 ? T[tr] : "?");
  snprintf(M.slewRate,  sizeof M.slewRate,  "%s", sp >= 0 && sp < 10 ? R[sp] : "?");
  snprintf(M.hemi,      sizeof M.hemi,      "%s", hm ? "N" : "S");
  int xs = r[4] - '0';
  snprintf(M.gpsSrc,    sizeof M.gpsSrc,    "%s", xs >= 0 && xs < 3 ? X[xs] : "?");
}
static void decGEC(const char *r) {                // sDDDDDDDD RRRRRRRR # (0.01")
  if (strlen(r) < 17 || (r[0] != '+' && r[0] != '-')) return;
  double dec = sub2l(r, 1, 8) / 360000.0 * (r[0] == '-' ? -1 : 1);
  double raH = sub2l(r, 9, 8) / 360000.0 / 15.0;
  int rh = (int)raH, rm = (int)((raH - rh) * 60);
  int dd = (int)fabs(dec); int dm = (int)((fabs(dec) - dd) * 60);
  snprintf(M.raStr,  sizeof M.raStr,  "%02dh%02dm", rh, rm);
  snprintf(M.decStr, sizeof M.decStr, "%c%02d %02d'", dec < 0 ? '-' : '+', dd, dm);
}
static void decAG(const char *r) {                 // gggg -> x0.01
  if (strlen(r) < 4 || !allDigits(r, 4)) return;
  snprintf(M.guideStr, sizeof M.guideStr, "%.2f/%.2f",
           sub2l(r, 0, 2) / 100.0, sub2l(r, 2, 2) / 100.0);
}

void mountQueryGLT() {
  char r[40];
  if (askMount(":GLT#", r, sizeof r, 600)) decGLT(r);
}

// ---- idle self-poll state machine --------------------------------
enum PollStep { P_MODEL, P_PROTO, P_FW1, P_FW2, P_GLT, P_GG, P_GT, P_GAS, P_GEC, P_AG, P_COUNT };
static uint8_t  pollStep    = P_MODEL;
static uint8_t  slowCounter = 0;

void mountRequestTimeRefresh() { pollStep = P_GLT; }

void mountPoll() {
  char r[40];
  switch (pollStep) {
    case P_MODEL:
      if (askMount(":MountInfo#", r, sizeof r, 500, 4) == 4) {
        strncpy(M.model, r, 5);
        snprintf(M.modelName, sizeof M.modelName, "%s", modelName(r));
      }
      break;
    case P_PROTO:
      if (askMount(":V#", r, sizeof r, 400)) {
        char *h = strchr(r, '#'); if (h) *h = 0;
        strncpy(M.proto, r, sizeof M.proto - 1);
      }
      break;
    case P_FW1:
      if (askMount(":FW1#", r, sizeof r, 500) >= 13)
        snprintf(M.fwMain, sizeof M.fwMain, "%c%c-%c%c-%c%c", r[0],r[1],r[2],r[3],r[4],r[5]);
      break;
    case P_FW2:
      if (askMount(":FW2#", r, sizeof r, 500) >= 13)
        snprintf(M.fwMotor, sizeof M.fwMotor, "%c%c-%c%c-%c%c", r[0],r[1],r[2],r[3],r[4],r[5]);
      break;
    case P_GLT: if (askMount(":GLT#", r, sizeof r, 500)) decGLT(r); break;
    case P_GG:  if (askMount(":Gg#",  r, sizeof r, 400)) decGG(r);  break;
    case P_GT:  if (askMount(":Gt#",  r, sizeof r, 400)) decGT(r);  break;
    case P_GAS: if (askMount(":GAS#", r, sizeof r, 500)) decGAS(r); break;
    case P_GEC: if (askMount(":GEC#", r, sizeof r, 500)) decGEC(r); break;
    case P_AG:  if (askMount(":AG#",  r, sizeof r, 400)) decAG(r);  break;
  }
  M.lastOkMs = millis();
  M.everOk = true;
  do {
    pollStep = (pollStep + 1) % P_COUNT;
    if (pollStep == P_MODEL) slowCounter++;
  } while ((pollStep == P_MODEL || pollStep == P_PROTO ||
            pollStep == P_FW1   || pollStep == P_FW2) && (slowCounter & 0x07));
}

void mountBegin() {
  Rs232.setRxBufferSize(UART_RX_BUFFER);
  Rs232.begin(BRIDGE_BAUD, SERIAL_8N1, RS232_RX_PIN, RS232_TX_PIN);
}
