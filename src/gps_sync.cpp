// gps_sync.cpp - push GPS date/time + site into the mount (auto + manual).
#include "gps_sync.h"
#include "config.h"
#include "gps.h"
#include "mount.h"
#include "bt_bridge.h"
#include "message.h"
#include "timeutil.h"
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

// ---- the actual sync -------------------------------------------------
static void doGpsSync(bool manual) {
  if (btConnected) {
    if (manual) setMsg("BT client active - cannot sync", C_WARN, 4000);
    snprintf(syncMsg, sizeof syncMsg, "sync held (BT active)"); syncMsgColor = C_WARN;
    return;
  }
  if (!gpsLocked()) {
    if (manual) setMsg("GPS NOT LOCKED - cannot sync", C_BAD, 4000);
    return;
  }

  setMsg("Syncing mount from GPS...", C_WARN, 9000);

  char r[40], c[24];
  if (askMount(":GLT#", r, sizeof r, 600)) LOG("[sync] before: :GLT# -> %s\n", r);

  // zone + DST first, so the local time below is converted back to true UTC
  long tz = gpsSyncTzEffective();
  int ok = 0;
  snprintf(c, sizeof c, ":SG%c%03ld#", tz < 0 ? '-' : '+', labs(tz));
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;
  LOG("[sync] %s -> %s\n", c, r);
  if (askMount(":SDS0#", r, sizeof r, 700) && r[0] == '1') ok++;
  LOG("[sync] :SDS0# -> %s\n", r);

  long ep  = daysFromCivil(gps.date.year(), gps.date.month(), gps.date.day()) * 86400L
           + gps.time.hour() * 3600L + gps.time.minute() * 60L + gps.time.second();
  long loc = ep + tz * 60L;
  int Y, Mo, D; civilFromDays(loc / 86400, Y, Mo, D);
  long tod = loc % 86400; if (tod < 0) tod += 86400;
  int hh = tod / 3600, mi = (tod % 3600) / 60, ss = tod % 60;

  // SmartEQ Pro (:V# V1.00) takes the site in whole arc-seconds, 6 digits. The
  // 8-digit 0.01" form is accepted ('1') but wraps to a garbage site.
  long lonAs = llround(gps.location.lng() * 3600.0);
  long latAs = llround(gps.location.lat() * 3600.0);

  snprintf(c, sizeof c, ":SC%02d%02d%02d#", Y % 100, Mo, D);
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;
  snprintf(c, sizeof c, ":SL%02d%02d%02d#", hh, mi, ss);
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;
  snprintf(c, sizeof c, ":Sg%c%06ld#", lonAs < 0 ? '-' : '+', labs(lonAs));
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;
  snprintf(c, sizeof c, ":St%c%06ld#", latAs < 0 ? '-' : '+', labs(latAs));
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;

  LOG("[sync] %04d-%02d-%02d %02d:%02d:%02d local (UTC%+ld min)  lat %.5f lon %.5f  ok=%d/6\n",
      Y, Mo, D, hh, mi, ss, tz, gps.location.lat(), gps.location.lng(), ok);
  if (askMount(":GLT#", r, sizeof r, 600)) LOG("[sync] after:  :GLT# -> %s\n", r);
  if (askMount(":GLS#", r, sizeof r, 600)) LOG("[sync] after:  :GLS# -> %s\n", r);
  if (askMount(":GAC#", r, sizeof r, 600)) LOG("[sync] after:  :GAC# -> %s\n", r);

  if (ok == 6) {
    setMsg("GPS sync OK", C_OK, 6000);
    lastSyncMs = millis();
    snprintf(syncMsg, sizeof syncMsg, "synced %02d:%02d:%02dZ",
             gps.time.hour(), gps.time.minute(), gps.time.second());
    syncMsgColor = C_OK;
  } else {
    snprintf(c, sizeof c, "GPS sync incomplete %d/6", ok); setMsg(c, C_WARN, 8000);
    snprintf(syncMsg, sizeof syncMsg, "sync failed %d/6 - retrying", ok);
    syncMsgColor = C_BAD;
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

  if (btConnected) {
    snprintf(syncMsg, sizeof syncMsg, "sync held (BT active)"); syncMsgColor = C_WARN;
    return;
  }
  if (!syncArmed) return;

  uint32_t held = now - lockSinceMs;
  if (held < GPS_LOCK_SETTLE_MS) {
    snprintf(syncMsg, sizeof syncMsg, "fix ok - syncing in %lus",
             (unsigned long)((GPS_LOCK_SETTLE_MS - held) / 1000 + 1));
    syncMsgColor = C_WARN;
    return;
  }
  syncArmed = false;
  doGpsSync(false);
}

// ---- manual re-sync (UI) -------------------------------------------
static volatile bool manualReq = false;

void gpsSyncRequestManual() { manualReq = true; }

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
