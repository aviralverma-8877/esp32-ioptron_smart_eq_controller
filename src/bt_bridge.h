// bt_bridge.h - Classic Bluetooth SPP endpoint and the byte bridge: transparent
// (iOptron) or through the LX200 / NexStar translator (xlate.h, proto.h).
//
// Open like a serial Bluetooth module: always discoverable and connectable,
// SSP "Just Works" pairing (no code, no button), any device may connect. A
// device whose old pairing we no longer have simply pairs again. Bonds are kept
// by Bluedroid in NVS (survive reboots); device names are saved alongside.
#pragma once
#include <stdint.h>
#include <stddef.h>

extern volatile bool btConnected;   // a client is connected -> stay transparent

void btBegin();
void btTick();                       // every loop: connect / pairing bookkeeping
void btBridgeTick();                 // when connected: pump both directions (or translate, xlate.h)
void btWrite(const uint8_t *buf, size_t n);   // to the connected client (translator replies)

// ---- stored (bonded) devices ----
int  btBondCount();
bool btBondGet(int i, uint8_t mac[6], char *name, size_t n);   // name or "AA:BB:.."
bool btBondForget(int i);
void btBondForgetAll();
uint32_t btBondSerial();             // bumps on every bond-list change (UI redraw)
