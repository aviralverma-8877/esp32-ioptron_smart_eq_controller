// Host test for src/xlate.cpp against a mock iOptron mount (no hardware).
//   ./run.sh            (Linux / WSL / macOS, needs g++)
#include "xlate.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- mock mount + client ----------------------------------------------------------
static std::vector<std::string> sent;     // iOptron commands the translator issued
static std::string reply;                 // bytes sent back to the client
static uint32_t    nowMs = 1000;
static std::string gec = "+1485686302564233#";      // Dec +41.269064 (0.01"), RA 0.712287 h (ms)
static std::string gas = "011221#";                 // tracking, N hemisphere
static char        msReply = '1';
static bool        mountUp = true;

static size_t mAsk(const char *cmd, char *out, size_t n, uint16_t to, uint8_t fixedLen) {
  sent.push_back(cmd);
  std::string r;
  if (!mountUp || to == 0) r = "";
  else if (!strcmp(cmd, ":GEC#")) r = gec;
  else if (!strcmp(cmd, ":GAS#")) r = gas;
  else if (!strcmp(cmd, ":MS#"))  r = std::string(1, msReply);
  else if (fixedLen == 1)         r = "1";
  if (fixedLen && r.size() > fixedLen) r.resize(fixedLen);
  if (r.size() > n - 1) r.resize(n - 1);
  memcpy(out, r.c_str(), r.size() + 1);
  return r.size();
}
static void mWrite(const uint8_t *b, size_t n) { reply.append((const char *)b, n); }
static uint32_t mMillis() { return nowMs; }
static bool mClock(long &utc, long &off) { utc = 1792531800L; off = 330; return true; }   // 2026-10-20 21:30:00Z, IST
static bool mSite(double &lat, double &lon) { lat = 28.6127; lon = 77.2295; return true; }
static bool mAltAz(double, double, double &alt, double &az) { alt = -12.5; az = 287.25; return true; }
static void mLog(const char *fmt, ...) { (void)fmt; }
static long   gotUtc = 0, gotOff = 0;            // last setClock / setSite from the translator
static double gotLat = 0, gotLon = 0;
static int    clockCalls = 0, siteCalls = 0;
static bool   gpsFix = false;                     // true: glue refuses (GPS owns time / site)
static bool mSetClock(long utc, long off) { clockCalls++; gotUtc = utc; gotOff = off; return !gpsFix; }
static bool mSetSite(double lat, double lon) { siteCalls++; gotLat = lat; gotLon = lon; return !gpsFix; }

// ---- helpers ------------------------------------------------------------------------
static int fails = 0, checks = 0;
static void check(bool ok, const char *what, const std::string &got = "") {
  checks++;
  if (!ok) { fails++; printf("FAIL  %s   got [%s]\n", what, got.c_str()); }
}
static std::string esc(const std::string &s) {
  std::string o;
  for (unsigned char c : s) {
    char b[8];
    if (c >= 32 && c < 127) { o += (char)c; continue; }
    snprintf(b, sizeof b, "\\x%02X", c); o += b;
  }
  return o;
}
static std::string send(const std::string &s) {
  reply.clear(); sent.clear();
  for (unsigned char c : s) xlateFeed(c);
  return reply;
}
static void expect(const std::string &in, const std::string &want) {
  std::string got = send(in);
  std::string what = "'" + esc(in) + "' -> '" + esc(want) + "'";
  check(got == want, what.c_str(), esc(got));
}
static bool sentHas(const char *c) {
  for (auto &s : sent) if (s == c) return true;
  return false;
}
static void expectSent(const std::vector<std::string> &want, const char *what) {
  std::string all;
  for (auto &s : sent) all += s + " ";
  check(sent == want, what, all);
}
static void reset() { xlateReset(); nowMs += 10000; msReply = '1'; mountUp = true; gas = "011221#"; }

// ---- tests ----------------------------------------------------------------------------
static void testLx200() {
  xlateSetMode(XL_LX200); reset();
  // position: RA 2564233 ms = 00:42:44.2 ; Dec 41.269064 = +41*16'08.6"
  expect("#:GR#", "00:42:44#");
  expect("#:GD#", "+41*16:09#");
  expect(":U#", "");
  expect(":GR#", "00:42.7#");
  expect(":GD#", "+41*16#");
  expect(":P#", "HIGH PRECISION");
  gec = "-0012345601234567#";                    // Dec -0.342933, RA 0.342935 h
  nowMs += 1000;
  expect(":GD#", "-00*20:35#");
  expect(":GR#", "00:20:35#");
  expect(":GA#", "-12*30:00#");
  expect(":GZ#", "287*15:00#");

  // Stellarium GoTo sequence
  expect("#:Sr 05:35:17#", "1");
  expect("#:Sd -05*23:28#", "1");
  expect("#:MS#", "0");
  // 5h35m17s = 20117000 ms ; -5d23'28" = -19408" = -1940800 (0.01")
  expectSent({":ST1#", ":Sr20117000#", ":Sd-01940800#", ":MS#"}, "LX200 GoTo -> iOptron sequence");
  expect(":Sr12:34.5#", "1");                    // short format
  expect(":Sd+89\xDF" "15#", "1");               // Meade degree byte
  expect(":Sr25:00:00#", "0");
  expect(":Sd+91*00:00#", "0");
  expect(":Sd+45*30'15#", "1");
  msReply = '0';
  expect(":MS#", "1Object below horizon#");
  mountUp = false;
  expect(":MS#", "2Mount did not answer#");
  mountUp = true; msReply = '1';
  expect(":CM#", "Coordinates     matched.        #");
  check(sentHas(":CM#") && sentHas(":Sr45270000#"), "sync sends :Sr :Sd :CM#");

  // motion / rates / stop
  expect(":RS#:Me#", "");
  expectSent({":SR9#", ":me#"}, "rate + move east");
  expect(":Qe#", ""); check(sentHas(":qR#"), "stop east -> :qR#");
  expect(":Qn#", ""); check(sentHas(":qD#"), "stop north -> :qD#");
  expect(":Q#", "");  check(sentHas(":q#"), "stop -> :q#");
  expect(":D#", "#");
  gas = "021221#";
  nowMs += 1000;
  expect(":D#", "\x7f#");
  expect(":Te#", "1");
  expect(":GW#", "GN1#");

  // ACK, info, time and site
  expect("\x06", "P");
  expect(":GVP#", "SmartEQ-RJ9#");
  expect(":GL#", "03:00:00#");                   // 21:30Z + 5:30
  expect(":GC#", "10/21/26#");
  expect(":GG#", "-05.5#");
  expect(":Gt#", "+28*37#");
  expect(":Gg#", "282*46#");                     // 77.2295 E = 282.7705 W
  expect(":Gc#", "24#");
  // client time: :SG -05.5 (= UTC+5:30), :SL 03:00:00, :SC 10/21/26 -> 2026-10-20 21:30:00Z
  clockCalls = 0;
  expect(":SG-05.5#", "1");
  expect(":SL03:00:00#", "1");
  check(clockCalls == 0, "LX200 clock waits for the date");
  expect(":SC10/21/26#", "1Updating Planetary Data#                              #");
  check(clockCalls == 1 && gotUtc == 1792531800L && gotOff == 330, "LX200 :SG :SL :SC -> setClock",
        std::to_string(gotUtc) + " " + std::to_string(gotOff));
  expect(":SC10/21/26#", "1Updating Planetary Data#                              #");
  check(clockCalls == 1, "date alone does not re-apply");
  expect(":SC13/45/26#", "1Updating Planetary Data#                              #");
  expect(":SL25:00:00#", "0");
  siteCalls = 0;
  expect(":St+28*37#", "1");
  check(siteCalls == 1 && fabs(gotLat - 28.61667) < 1e-4 && std::isnan(gotLon), "LX200 :St -> lat only");
  expect(":Sg282*46#", "1");                     // west-positive 282d46' = 77d14' E
  check(siteCalls == 2 && std::isnan(gotLat) && fabs(gotLon - 77.23333) < 1e-4, "LX200 :Sg -> lon east",
        std::to_string(gotLon));
  expect(":Sg098*30#", "1");
  check(fabs(gotLon + 98.5) < 1e-4, "LX200 :Sg west", std::to_string(gotLon));
  expect(":XYZ#", "");                           // unknown: silent

  // framing robustness
  expect(":G", "");
  nowMs += 2000; xlateTick();                    // stale partial dropped
  expect(":GVN#", "1.0#");
  expect(":GV", ""); expect("N#", "1.0#");       // split across reads
}

static void testNexStar() {
  xlateSetMode(XL_NEXSTAR); reset();
  gec = "+1485686302564233#";
  // RA 0.712287h / 24 * 65536 = 1945.0 -> 0x0799 ; Dec 41.269064 / 360 * 65536 = 7512.8 -> 0x1D59
  expect("E", "0799,1D59#");
  std::string e = send("e");
  check(e.size() == 18 && e.substr(0, 4) == "0799" && e.substr(6, 2) == "00" && e.substr(15) == "00#",
        "precise e format", esc(e));
  gec = "-0012345601234567#";
  nowMs += 1000;
  std::string ne = send("E");
  check(ne.substr(5) == "FFC2#", "negative Dec wraps", ne);   // 65536 - 0.342933/360*65536 = 65473.6 -> FFC2
  expect("Kx", "x#");
  expect("V", std::string("\x04\x15#", 3));
  expect("m", std::string("\x14#", 2));
  expect("J", std::string("\x01#", 2));
  expect("L", "0#");
  gas = "021221#"; nowMs += 1000;
  expect("L", "1#");
  gas = "011221#"; nowMs += 1000;
  expect("t", std::string("\x02#", 2));
  gas = "001220#"; nowMs += 1000;
  expect("t", std::string("\x00#", 2));
  expect("T\x02", "#"); check(sentHas(":ST1#"), "T2 -> :ST1#");
  expect(std::string("T\x00", 2), "#"); check(sentHas(":ST0#"), "T0 -> :ST0#");

  // GoTo 16-bit: RA 0x4000 = 6h, Dec 0x1000 = 22.5 deg
  expect("R4000,1000", "#");
  expectSent({":ST1#", ":Sr21600000#", ":Sd+08100000#", ":MS#"}, "NexStar R GoTo");
  // precise, southern Dec: 0xF0000000 = -22.5 deg
  expect("r80000000,F0000000", "#");
  expectSent({":ST1#", ":Sr43200000#", ":Sd-08100000#", ":MS#"}, "NexStar r GoTo south");
  expect("s80000000,F0000000", "#");
  check(sentHas(":CM#") && !sentHas(":MS#"), "NexStar s sync");
  expect("R4000,5000", "#");                     // 112.5 deg: invalid -> not sent
  check(!sentHas(":MS#"), "invalid Dec not slewed");
  expect("M", "#"); check(sentHas(":q#"), "M -> :q#");

  // passthrough slews: fixed rate 9 RA positive, then stop; Dec negative rate 4
  expect(std::string("P\x02\x10\x24\x09\x00\x00\x00", 8), "#");
  expectSent({":SR9#", ":me#"}, "P fixed RA+ rate 9");
  expect(std::string("P\x02\x10\x24\x00\x00\x00\x00", 8), "#");
  expectSent({":qR#"}, "P fixed RA stop");
  expect(std::string("P\x02\x11\x25\x04\x00\x00\x00", 8), "#");
  expectSent({":SR4#", ":ms#"}, "P fixed Dec- rate 4");
  // variable: 16x sidereal = 240.66"/s = 963 quarter-arcsec = 0x03C3
  expect(std::string("P\x03\x11\x06\x03\xC3\x00\x00", 8), "#");
  expectSent({":SR4#", ":mn#"}, "P variable Dec+ 16x");
  expect(std::string("P\x01\x10\xFE\x00\x00\x00\x02", 8), std::string("\x07\x0B#", 3));

  // time + site
  // 21:30Z, zone +5:30: W must be whole hours (5), so local is reported as
  // UTC + 5:00 = 02:30 10/21/26 - apps compute UTC = local - W and get 21:30Z
  expect("h", std::string("\x02\x1E\x00\x0A\x15\x1A\x05\x00#", 9));
  expect("w", std::string("\x1C\x24\x2E\x00\x4D\x0D\x2E\x00#", 9));   // 28 36 46 N 77 13 46 E
  // Stellarium Mobile's real H: 19:32:36 10/20/26, offset +5 (IST truncated), no DST.
  // Our zone is +5:30 -> keep it: UTC = 19:32:36 - 5:30 = 14:02:36Z
  clockCalls = 0;
  expect(std::string("H\x13\x20\x24\x0A\x14\x1A\x05\x00", 9), "#");
  check(clockCalls == 1 && gotOff == 330 && gotUtc == 1792504956L, "NexStar H (IST) -> setClock",
        std::to_string(gotUtc) + " " + std::to_string(gotOff));
  check(sent.empty(), "translator itself sends nothing to the mount for H");
  expect(std::string("H\x0C\x00\x00\x07\x04\x1A\xFB\x01", 9), "#");   // 12:00 7/4/26, UTC-5 + DST (US EDT)
  check(gotOff == -240 && gotUtc == 1783180800L, "NexStar H west + DST",
        std::to_string(gotUtc) + " " + std::to_string(gotOff));
  std::string hd = send("h");
  check(hd.size() == 9 && hd[7] == 1, "h echoes the client's DST flag", esc(hd));
  expect(std::string("H\x13\x20\x24\x0D\x14\x1A\x05\x00", 9), "#");   // month 13: rejected
  check(clockCalls == 2, "bad H not applied");
  siteCalls = 0;
  expect(std::string("W\x0D\x01\x0A\x00\x4D\x2B\x18\x00", 9), "#");   // 13 01 10 N, 77 43 24 E
  check(siteCalls == 1 && fabs(gotLat - 13.01944) < 1e-4 && fabs(gotLon - 77.72333) < 1e-4, "NexStar W -> setSite");
  expect(std::string("W\x21\x00\x00\x01\x76\x00\x00\x01", 9), "#");   // 33 S, 118 W
  check(gotLat == -33.0 && gotLon == -118.0, "NexStar W south / west");
  gpsFix = true;
  expect(std::string("H\x13\x20\x24\x0A\x14\x1A\x05\x00", 9), "#");   // glue refuses: still acked
  gpsFix = false;
  expect("Z", "CC44,F71C#");                     // az 287.25, alt -12.5

  // junk + split + stale partial
  expect(std::string("\x00\xFF" "q", 3), "");
  expect("R40", ""); nowMs += 2000; xlateTick();
  expect("J", std::string("\x01#", 2));
  expect("K", ""); expect("y", "y#");
}

static void testNativeAndTracking() {
  xlateSetMode(XL_NATIVE); reset();
  expect(":GR#E", "");                           // native: translator ignores input
  check(sent.empty(), "native mode does nothing");
  check(!strcmp(xlateModeName(XL_LX200), "LX200") && !strcmp(xlateModeName(XL_NATIVE), "iOptron"), "names");

  // post-GoTo: slew seen, ends stopped -> tracking restarted
  xlateSetMode(XL_LX200); reset();
  send(":Sr01:00:00#:Sd+10*00:00#:MS#");
  gas = "021221#"; nowMs += 1100; sent.clear(); xlateTick();
  check(sentHas(":GAS#") && !sentHas(":ST1#"), "watch: still slewing");
  gas = "001221#"; nowMs += 1100; sent.clear(); xlateTick();
  check(sentHas(":ST1#"), "watch: stopped after slew -> :ST1#");
  sent.clear(); nowMs += 1100; xlateTick();
  check(sent.empty(), "watch ends");
}

int main() {
  XlateIo io = { mAsk, mWrite, mMillis, mClock, mSite, mAltAz, mLog, mSetClock, mSetSite };
  xlateBegin(io);
  testLx200();
  testNexStar();
  testNativeAndTracking();
  printf("%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
