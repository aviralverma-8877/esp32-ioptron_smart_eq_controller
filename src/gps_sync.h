// gps_sync.h - push GPS date/time + site into the mount.
// Automatic once a fix is stable; the UI can force a manual re-sync. Never runs
// while a BT client is connected (the bridge must stay transparent).
#pragma once
#include <stdint.h>

extern char     syncMsg[30];        // display-ready status ("synced 12:04:33Z", ...)
extern uint16_t syncMsgColor;

void gpsSyncBegin();
void gpsSyncTick();                  // call every loop: auto-sync
void gpsSyncRequestManual();         // UI "Sync GPS now" (runs on next tick)
