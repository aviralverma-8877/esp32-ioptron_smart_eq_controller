// proto.cpp - ESP32 glue for the LX200 / NexStar translator (logic in xlate.cpp).
#include "proto.h"
#include "config.h"
#include "mount.h"
#include "bt_bridge.h"
#include "gps_sync.h"
#include "message.h"
#include "sky.h"
#include <Preferences.h>
#include <math.h>
#include <stdarg.h>

static size_t ioAsk(const char *cmd, char *out, size_t n, uint16_t to, uint8_t fixedLen) {
  return askMount(cmd, out, n, to, fixedLen);
}
static void     ioWrite(const uint8_t *b, size_t n) { btWrite(b, n); }
static uint32_t ioMillis() { return millis(); }

// UTC from GPS (or the mount's clock); offset = what the mount displays local
// time with (GPS sync writes the zone with DST off), else the zone setting.
static bool ioClock(long &utc, long &off) {
  double jd;
  if (!skyNowJD(jd)) return false;
  utc = lround((jd - 2440587.5) * 86400.0);
  off = M.timeValid ? M.offsetMin + M.dstFlag * 60 : gpsSyncTzEffective();
  return true;
}
static bool ioSite(double &lat, double &lon) { return skySite(lat, lon); }
static bool ioAltAz(double ra, double dec, double &alt, double &az) {
  return skyRaDecToAltAz(ra, dec, alt, az);
}
static void ioLog(const char *fmt, ...) {
#if defined(DEBUG_LOG) && DEBUG_LOG
  char b[160];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  Serial.print(b);
#else
  (void)fmt;
#endif
}

void protoSetMode(XlateMode m) {
  if (m >= XL_COUNT) m = XL_NATIVE;
  xlateSetMode(m);
  Preferences p;
  if (p.begin("smarteq", false)) { p.putUChar("proto", m); p.end(); }
  LOG("[proto] client protocol: %s\n", xlateModeName(m));
}

void protoCycle() {
  protoSetMode((XlateMode)((xlateMode() + 1) % XL_COUNT));
  char b[30];
  snprintf(b, sizeof b, "BT protocol: %s", xlateModeName(xlateMode()));
  setMsg(b, C_OK, 3000);
}

void protoBegin() {
  static const XlateIo io = { ioAsk, ioWrite, ioMillis, ioClock, ioSite, ioAltAz, ioLog };
  xlateBegin(io);
  uint8_t m = XL_NATIVE;
  Preferences p;
  if (p.begin("smarteq", true)) { m = p.getUChar("proto", XL_NATIVE); p.end(); }
  xlateSetMode(m < XL_COUNT ? (XlateMode)m : XL_NATIVE);
  LOG("[proto] client protocol: %s\n", xlateModeName(xlateMode()));
}
