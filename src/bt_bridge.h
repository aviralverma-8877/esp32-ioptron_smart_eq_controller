// bt_bridge.h - Classic Bluetooth SPP endpoint and the transparent byte bridge.
//
// The device is never discoverable except during a pairing window opened from
// the UI. Pairing uses SSP numeric comparison, accepted on the controller.
// Bonds are kept by Bluedroid in NVS (survive reboots); device names are saved
// alongside. Only bonded devices may open the SPP link.
#pragma once
#include <stdint.h>
#include <stddef.h>

extern volatile bool btConnected;   // a client is connected -> stay transparent

void btBegin();
void btTick();                       // every loop: pairing timeout, bond bookkeeping
void btBridgeTick();                 // when connected: pump both directions

// ---- pairing window ----
void     btPairStart(uint32_t ms);   // discoverable + accept new pairings for `ms`
void     btPairStop();
bool     btPairing();
uint32_t btPairLeftMs();
bool     btConfirmPending(uint32_t *code);   // peer waits for numeric-comparison OK
void     btConfirmReply(bool accept);

// ---- stored (bonded) devices ----
int  btBondCount();
bool btBondGet(int i, uint8_t mac[6], char *name, size_t n);   // name or "AA:BB:.."
bool btBondForget(int i);
void btBondForgetAll();
uint32_t btBondSerial();             // bumps on every bond-list change (UI redraw)
