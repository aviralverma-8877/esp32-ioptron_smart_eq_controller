// gps_sync.h - push GPS date/time + site into the mount.
// Automatic once a fix is stable (and the mount answers); a failed sync is
// retried after a minute; the UI can force a manual re-sync. Held while a BT
// client uses the transparent iOptron bridge; runs between client commands in
// LX200 / NexStar mode, where the controller owns the line.
#pragma once
#include <stdint.h>

extern char     syncMsg[30];        // display-ready status ("synced 12:04:33Z", ...)
extern uint16_t syncMsgColor;

void gpsSyncBegin();
void gpsSyncTick();                  // call every loop: auto-sync
void gpsSyncRequestManual();         // UI "Sync GPS now" (runs on next tick)

// Time zone written to the mount on each sync (:SG, DST always off). Either
// automatic (from the GPS position) or a fixed offset saved in NVS.
#define TZ_AUTO  (-9999)
#define TZ_MIN   (-720)              // UTC-12:00
#define TZ_MAX   (840)               // UTC+14:00
int  gpsSyncTzSetting();             // TZ_AUTO or fixed minutes
int  gpsSyncTzAuto();                // what "auto" resolves to right now
int  gpsSyncTzEffective();           // offset the next sync will use
void gpsSyncTzSet(int minutes);      // TZ_AUTO or minutes; saved, re-arms the sync
void gpsSyncFactoryReset();          // erase the saved settings (NVS "smarteq")

// Raw mount writers, also used for time / site a Bluetooth app sends (proto.cpp).
// Clock: :SG zone, :SDS0 (DST off), :SC date, :SL local time -> returns # OK of 4.
// Site: :Sg / :St in arc-seconds; NAN skips that one -> returns # OK.
int  gpsSyncWriteClock(long utcEpoch, long tzMin);
int  gpsSyncWriteSite(double latDeg, double lonDeg);
