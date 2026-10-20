// gps_sync.cpp - push GPS date/time + site into the mount (auto + manual).
#include "gps_sync.h"
#include "config.h"
#include "gps.h"
#include "mount.h"
#include "bt_bridge.h"
#include "message.h"
#include "timeutil.h"
#include "xlate.h"
#include <Preferences.h>
#include <math.h>
#include <string.h>

#define GPS_LOCK_SETTLE_MS   6000UL
#define GPS_RESYNC_MS        (3UL * 3600 * 1000)   // re-sync every 3 h to trim drift

char     syncMsg[30]  = "waiting for GPS fix";
uint16_t syncMsgColor = C_LBL;

static bool     syncArmed   = true;    // re-armed on fix loss / periodic timer
static uint32_t lockSinceMs = 0;       // millis when the present fix began
static uint32_t lastSyncMs  = 0;       // millis of last OK sync (0 = never)
static uint32_t failMs      = 0;       // millis of last failed auto sync (0 = none pending)
#define GPS_RETRY_MS         60000UL   // failed auto sync: try again after this

// A transparent (iOptron) bridge must not get our commands mixed into the
// client's stream; in LX200 / NexStar mode the controller owns the line, so
// the sync runs between client commands.
static bool btHolds() { return btConnected && xlateMode() == XL_NATIVE; }

// ---- time zone from GPS position ---------------------------------------
// The mount keeps UTC internally as (local - offset - DST), so the offset we
// write and the local time we send must agree. Standard (no-DST) offset from a
// coarse box table of the off-meridian zones, first match wins; elsewhere the
// nautical zone round(lon / 15). Only the mount's local-clock display depends
// on the table being right near borders - its UTC is correct either way.
static long tzOffsetMinutes(double lat, double lon) {
  static const struct { float la0, la1, lo0, lo1; int16_t min; } Z[] = {
    { 26.3f, 30.5f,  80.0f,  88.2f,  345 },   // Nepal
    { 26.7f, 28.4f,  88.7f,  92.2f,  360 },   // Bhutan
    { 20.5f, 26.7f,  88.0f,  92.7f,  360 },   // Bangladesh
    {  5.8f,  9.9f,  79.5f,  82.0f,  330 },   // Sri Lanka
    { 33.0f, 38.5f,  60.5f,  71.5f,  270 },   // Afghanistan
    { 23.5f, 37.1f,  60.8f,  71.0f,  300 },   // Pakistan
    {  6.5f, 28.0f,  68.0f,  97.5f,  330 },   // India
    { 28.0f, 35.7f,  72.5f,  81.0f,  330 },   // India (north)
    { 25.0f, 39.8f,  44.0f,  61.0f,  210 },   // Iran
    {  9.5f, 28.6f,  92.2f, 101.2f,  390 },   // Myanmar
    { 18.0f, 53.6f,  73.5f, 135.0f,  480 },   // China
    {-38.0f,-10.9f, 129.0f, 141.0f,  570 },   // Australia NT / SA
    { 46.5f, 52.0f, -59.5f, -52.5f, -210 },   // Newfoundland
  };
  for (auto &z : Z)
    if (lat >= z.la0 && lat <= z.la1 && lon >= z.lo0 && lon <= z.lo1) return z.min;
  return lround(lon / 15.0) * 60;
}

static int tzSetting = TZ_AUTO;         // NVS "smarteq"/"tz"

int gpsSyncTzSetting() { return tzSetting; }

int gpsSyncTzAuto() {
  if (gps.location.isValid()) return tzOffsetMinutes(gps.location.lat(), gps.location.lng());
  if (M.siteValid) {
    return tzOffsetMinutes(M.latDeg, M.lonDeg);
  }
  return 0;
}

int gpsSyncTzEffective() { return tzSetting == TZ_AUTO ? gpsSyncTzAuto() : tzSetting; }

void gpsSyncTzSet(int minutes) {
  if (minutes != TZ_AUTO) minutes = constrain(minutes, TZ_MIN, TZ_MAX);
  tzSetting = minutes;
  Preferences p;
  if (p.begin("smarteq", false)) { p.putInt("tz", minutes); p.end(); }
  syncArmed = true;                     // push it on the next locked tick
  LOG("[sync] time zone set: %d%s\n", minutes, minutes == TZ_AUTO ? " (auto)" : " min");
}

// ---- mount writers (GPS sync + time / site sent by a Bluetooth app) -------
static bool ask1(const char *c) {                 // setters answer a bare '1' (no '#')
  char r[8];
  bool ok = askMount(c, r, sizeof r, 700, 1) == 1 && r[0] == '1';
  LOG("[sync] %s -> %s\n", c, ok ? "1" : "FAIL");
  return ok;
}

int gpsSyncWriteClock(long utcEpoch, long tzMin) {
  // zone + DST first, so the local time below is converted back to true UTC
  uint32_t t0 = millis();
  char c[24];
  int ok = 0;
  snprintf(c, sizeof c, ":SG%c%03ld#", tzMin < 0 ? '-' : '+', labs(tzMin));
  ok += ask1(c);
  ok += ask1(":SDS0#");
  // the clock moved on while the zone was written: send the time as of :SL
  long loc = utcEpoch + (long)((millis() - t0 + 500) / 1000) + tzMin * 60L;
  long days = loc / 86400, tod = loc % 86400;
  if (tod < 0) { tod += 86400; days--; }
  int Y, Mo, D; civilFromDays(days, Y, Mo, D);
  snprintf(c, sizeof c, ":SC%02d%02d%02d#", Y % 100, Mo, D);
  ok += ask1(c);
  snprintf(c, sizeof c, ":SL%02ld%02ld%02ld#", tod / 3600, tod / 60 % 60, tod % 60);
  ok += ask1(c);
  return ok;
}

int gpsSyncWriteSite(double latDeg, double lonDeg) {
  // SmartEQ Pro (:V# V1.00) takes the site in whole arc-seconds, 6 digits. The
  // 8-digit 0.01" form is accepted ('1') but wraps to a garbage site.
  char c[24];
  int ok = 0;
  if (!isnan(lonDeg)) {
    long as = llround(lonDeg * 3600.0);
    snprintf(c, sizeof c, ":Sg%c%06ld#", as < 0 ? '-' : '+', labs(as));
    ok += ask1(c);
  }
  if (!isnan(latDeg)) {
    long as = llround(latDeg * 3600.0);
    snprintf(c, sizeof c, ":St%c%06ld#", as < 0 ? '-' : '+', labs(as));
    ok += ask1(c);
  }
  return ok;
}

// ---- the actual sync -------------------------------------------------
static void doGpsSync(bool manual) {
  if (btHolds()) {
    if (manual) setMsg("BT client active - cannot sync", C_WARN, 4000);
    snprintf(syncMsg, sizeof syncMsg, "sync held (BT active)"); syncMsgColor = C_WARN;
    return;
  }
  if (!gpsLocked()) {
    if (manual) setMsg("GPS NOT LOCKED - cannot sync", C_BAD, 4000);
    return;
  }
  if (!mountLinkUp()) {
    if (manual) setMsg("No mount link - cannot sync", C_BAD, 4000);
    return;
  }

  setMsg("Syncing mount from GPS...", C_WARN, 9000);

  char r[40], c[24];
  if (askMount(":GLT#", r, sizeof r, 600)) LOG("[sync] before: :GLT# -> %s\n", r);

  long tz = gpsSyncTzEffective();
  long ep = daysFromCivil(gps.date.year(), gps.date.month(), gps.date.day()) * 86400L
          + gps.time.hour() * 3600L + gps.time.minute() * 60L + gps.time.second();
  int ok = gpsSyncWriteClock(ep, tz) + gpsSyncWriteSite(gps.location.lat(), gps.location.lng());
  LOG("[sync] GPS %02d:%02d:%02dZ (UTC%+ld min)  lat %.5f lon %.5f  ok=%d/6\n",
      gps.time.hour(), gps.time.minute(), gps.time.second(), tz,
      gps.location.lat(), gps.location.lng(), ok);
  if (askMount(":GLT#", r, sizeof r, 600)) LOG("[sync] after:  :GLT# -> %s\n", r);
  if (askMount(":GLS#", r, sizeof r, 600)) LOG("[sync] after:  :GLS# -> %s\n", r);
  if (askMount(":GAC#", r, sizeof r, 600)) LOG("[sync] after:  :GAC# -> %s\n", r);

  if (ok == 6) {
    setMsg("GPS sync OK", C_OK, 6000);
    lastSyncMs = millis();
    failMs = 0;
    snprintf(syncMsg, sizeof syncMsg, "synced %02d:%02d:%02dZ",
             gps.time.hour(), gps.time.minute(), gps.time.second());
    syncMsgColor = C_OK;
  } else {
    snprintf(c, sizeof c, "GPS sync incomplete %d/6", ok); setMsg(c, C_WARN, 8000);
    snprintf(syncMsg, sizeof syncMsg, "sync failed %d/6 - retrying", ok);
    syncMsgColor = C_BAD;
    failMs = millis();                     // autoSyncTick re-arms after GPS_RETRY_MS
  }
  mountRequestTimeRefresh();               // re-read the displayed values
}

// fire the sync automatically once the fix has been stable long enough
static void autoSyncTick() {
  bool locked = gpsLocked();
  uint32_t now = millis();

  if (!locked) {
    // A marginal fix drops in and out; once synced, only the periodic timer
    // (or a time-zone change) re-arms - repeated :SL/:SC writes upset tracking.
    if (!lastSyncMs) syncArmed = true;
    lockSinceMs = 0;
    if (!lastSyncMs) { strcpy(syncMsg, "waiting for GPS fix"); syncMsgColor = C_LBL; }
    else { snprintf(syncMsg, sizeof syncMsg, "fix lost - was synced"); syncMsgColor = C_WARN; }
    return;
  }
  if (!lockSinceMs) lockSinceMs = now;
  if (lastSyncMs && now - lastSyncMs > GPS_RESYNC_MS) syncArmed = true;

  if (btHolds()) {
    snprintf(syncMsg, sizeof syncMsg, "sync held (BT active)"); syncMsgColor = C_WARN;
    return;
  }
  // a failed sync (mount busy / not answering) used to wait for the next fix
  // loss, i.e. forever under open sky - retry it after a minute instead
  if (!syncArmed && failMs && now - failMs > GPS_RETRY_MS) { failMs = 0; syncArmed = true; }
  if (!syncArmed) return;

  uint32_t held = now - lockSinceMs;
  if (held < GPS_LOCK_SETTLE_MS) {
    snprintf(syncMsg, sizeof syncMsg, "fix ok - syncing in %lus",
             (unsigned long)((GPS_LOCK_SETTLE_MS - held) / 1000 + 1));
    syncMsgColor = C_WARN;
    return;
  }
  if (!mountLinkUp()) {                    // stay armed: sync as soon as the mount answers
    snprintf(syncMsg, sizeof syncMsg, "fix ok - waiting for mount"); syncMsgColor = C_WARN;
    return;
  }
  syncArmed = false;
  doGpsSync(false);
}

// ---- manual re-sync (UI) -------------------------------------------
static volatile bool manualReq = false;

void gpsSyncRequestManual() { manualReq = true; }

void gpsSyncFactoryReset() {
  Preferences p;
  if (p.begin("smarteq", false)) { p.clear(); p.end(); }
  tzSetting = TZ_AUTO;
}

void gpsSyncBegin() {
  Preferences p;
  if (p.begin("smarteq", true)) { tzSetting = p.getInt("tz", TZ_AUTO); p.end(); }
  if (tzSetting != TZ_AUTO && (tzSetting < TZ_MIN || tzSetting > TZ_MAX)) tzSetting = TZ_AUTO;
}

void gpsSyncTick() {
  if (manualReq) {
    manualReq = false;
    LOG("[sync] manual request\n");
    syncArmed = false;
    doGpsSync(true);
  }
  static uint32_t t = 0;
  if (millis() - t > 500) { t = millis(); autoSyncTick(); }
}
