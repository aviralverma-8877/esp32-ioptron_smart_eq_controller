// gps_sync.h - push GPS date/time + site into the mount.
// Automatic once a fix is stable; BTN1 forces a manual re-sync. Never runs
// while a BT client is connected (the bridge must stay transparent).
#pragma once
#include <stdint.h>

extern char     syncMsg[30];        // display-ready status ("synced 12:04:33Z", ...)
extern uint16_t syncMsgColor;

void gpsSyncBegin();                 // attach the BTN1 interrupt
void gpsSyncTick();                  // call every loop: auto-sync + button
