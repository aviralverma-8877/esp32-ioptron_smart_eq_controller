// gps.h - NEO-6M NMEA input (UART1 RX-only) via TinyGPS++.
#pragma once
#include <stddef.h>
#include <TinyGPSPlus.h>

extern TinyGPSPlus gps;

void gpsBegin();
void gpsFeed();                          // pump UART bytes into the parser
bool gpsLocked();                        // valid, fresh 3D-ish fix (>=4 sats)
void gpsStatusStr(char *out, size_t n);  // "LOCKED 8 sat" / "acquiring 3 sat" / ...
