// gps_sync.cpp - push GPS date/time + site into the mount (auto + manual).
#include "gps_sync.h"
#include "config.h"
#include "gps.h"
#include "mount.h"
#include "bt_bridge.h"
#include "message.h"
#include "timeutil.h"
#include <math.h>
#include <string.h>

#define GPS_LOCK_SETTLE_MS   6000UL
#define GPS_RESYNC_MS        (3UL * 3600 * 1000)   // re-sync every 3 h to trim drift

char     syncMsg[30]  = "waiting for GPS fix";
uint16_t syncMsgColor = C_LBL;

static bool     syncArmed   = true;    // re-armed on fix loss / periodic timer
static uint32_t lockSinceMs = 0;       // millis when the present fix began
static uint32_t lastSyncMs  = 0;       // millis of last OK sync (0 = never)

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

  mountQueryGLT();                         // refresh mount's UTC offset + DST

  long ep  = daysFromCivil(gps.date.year(), gps.date.month(), gps.date.day()) * 86400L
           + gps.time.hour() * 3600L + gps.time.minute() * 60L + gps.time.second();
  long loc = ep + M.offsetMin * 60L + (M.dstFlag ? 3600L : 0L);
  int Y, Mo, D; civilFromDays(loc / 86400, Y, Mo, D);
  long tod = loc % 86400; if (tod < 0) tod += 86400;
  int hh = tod / 3600, mi = (tod % 3600) / 60, ss = tod % 60;

  long lon001 = llround(gps.location.lng() * 360000.0);   // 0.01 arc-seconds
  long lat001 = llround(gps.location.lat() * 360000.0);

  char r[40], c[24];
  int ok = 0;
  snprintf(c, sizeof c, ":SC%02d%02d%02d#", Y % 100, Mo, D);
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;
  snprintf(c, sizeof c, ":SL%02d%02d%02d#", hh, mi, ss);
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;
  snprintf(c, sizeof c, ":Sg%c%08ld#", lon001 < 0 ? '-' : '+', labs(lon001));
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;
  snprintf(c, sizeof c, ":St%c%08ld#", lat001 < 0 ? '-' : '+', labs(lat001));
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;

  LOG("[sync] %04d-%02d-%02d %02d:%02d:%02d local  lat %.5f lon %.5f  ok=%d/4\n",
      Y, Mo, D, hh, mi, ss, gps.location.lat(), gps.location.lng(), ok);

  if (ok == 4) {
    setMsg("GPS sync OK", C_OK, 6000);
    lastSyncMs = millis();
    snprintf(syncMsg, sizeof syncMsg, "synced %02d:%02d:%02dZ",
             gps.time.hour(), gps.time.minute(), gps.time.second());
    syncMsgColor = C_OK;
  } else {
    snprintf(c, sizeof c, "GPS sync incomplete %d/4", ok); setMsg(c, C_WARN, 8000);
    snprintf(syncMsg, sizeof syncMsg, "sync failed %d/4 - retrying", ok);
    syncMsgColor = C_BAD;
  }
  mountRequestTimeRefresh();               // re-read the displayed values
}

// fire the sync automatically once the fix has been stable long enough
static void autoSyncTick() {
  bool locked = gpsLocked();
  uint32_t now = millis();

  if (!locked) {
    syncArmed = true;
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

void gpsSyncBegin() {}

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
