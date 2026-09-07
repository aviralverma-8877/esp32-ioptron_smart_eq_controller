// bt_bridge.h - Classic Bluetooth SPP endpoint and the transparent byte bridge.
#pragma once

extern volatile bool btConnected;   // a client is connected -> stay transparent

void btBegin();
void btBridgeTick();                 // when connected: pump both directions
