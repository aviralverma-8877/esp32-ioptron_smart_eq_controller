// proto.cpp - ESP32 glue for the LX200 / NexStar translator (logic in xlate.cpp).
#include "proto.h"
#include "config.h"
#include "mount.h"
#include "bt_bridge.h"
#include "gps.h"
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
  // a client can connect before the idle poll ever read the mount's clock
  // (the poll stops while connected): ask the mount once, so its zone (e.g.
  // +5:30) is known before an app's whole-hour offset gets compared with it
  if (!M.timeValid && !gpsLocked()) { mountQueryGLT(); if (!M.siteValid) mountQuerySite(); }
  if (!skyNowJD(jd)) return false;
  utc = lround((jd - 2440587.5) * 86400.0);
  off = M.timeValid ? M.offsetMin + M.dstFlag * 60 : gpsSyncTzEffective();
  return true;
}
static bool ioSite(double &lat, double &lon) { return skySite(lat, lon); }
static bool ioAltAz(double ra, double dec, double &alt, double &az) {
  return skyRaDecToAltAz(ra, dec, alt, az);
}
// Time / site from the app: the GPS is the authority while it has a fix;
// without one (indoors, cold start) the phone's clock and position are used.
// M is re-read afterwards so the translator's own replies (h, :GL# ...) match.
static bool ioSetClock(long utc, long off) {
  if (gpsLocked()) return false;
  // Apps send the zone in whole hours (India +5:30 -> +5) with the true local
  // time. The authority for the zone is our setting (Auto: from the site), not
  // whatever the mount has stored - a wrong stored zone would otherwise keep
  // itself alive. Close to ours = the app rounded it: use ours and re-derive UTC.
  long tz = gpsSyncTzEffective();
  if (tz != off && labs(tz - off) < 60) { utc += (off - tz) * 60L; off = tz; }
  // apps re-send the time on reconnect / periodically: rewriting the clock
  // while tracking upsets the mount, so skip it when it already agrees
  double jd;
  if (M.timeValid && M.offsetMin + M.dstFlag * 60 == off && skyNowJD(jd) &&
      labs(lround((jd - 2440587.5) * 86400.0) - utc) <= 2) {
    LOG("[proto] app time matches the mount (zone %+ld min) - not rewritten\n", off);
    return true;
  }
  int ok = gpsSyncWriteClock(utc, off);
  mountQueryGLT();
  if (ok == 4) setMsg("Mount time set from app", C_OK, 4000);
  else         setMsg("App time: mount write failed", C_BAD, 4000);
  return ok == 4;
}
static bool ioSetSite(double lat, double lon) {
  if (gpsLocked()) return false;
  int want = !isnan(lat) + !isnan(lon);
  int ok = gpsSyncWriteSite(lat, lon);
  mountQuerySite();
  if (ok == want) setMsg("Mount site set from app", C_OK, 4000);
  else            setMsg("App site: mount write failed", C_BAD, 4000);
  return ok == want;
}

static void ioLog(const char *fmt, ...) {
#if defined(DEBUG_LOG) && DEBUG_LOG
  char b[160];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  dbgPuts(b);
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
  static const XlateIo io = { ioAsk, ioWrite, ioMillis, ioClock, ioSite, ioAltAz, ioLog,
                              ioSetClock, ioSetSite };
  xlateBegin(io);
  uint8_t m = XL_NATIVE;
  Preferences p;
  if (p.begin("smarteq", true)) { m = p.getUChar("proto", XL_NATIVE); p.end(); }
  xlateSetMode(m < XL_COUNT ? (XlateMode)m : XL_NATIVE);
  LOG("[proto] client protocol: %s\n", xlateModeName(xlateMode()));
}
