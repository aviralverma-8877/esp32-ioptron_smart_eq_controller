#include "gps.h"
#include "config.h"
#include <stdio.h>

static HardwareSerial GpsSer(1);
TinyGPSPlus gps;

void gpsBegin() {
  GpsSer.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, -1);   // receive only
}

void gpsFeed() {
  while (GpsSer.available()) gps.encode(GpsSer.read());
}

bool gpsLocked() {
  return gps.location.isValid() && gps.location.age() < 5000 &&
         gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024 &&
         gps.satellites.isValid() && gps.satellites.value() >= 4;
}

void gpsStatusStr(char *out, size_t n) {
  if (millis() > 8000 && gps.charsProcessed() < 20) { snprintf(out, n, "no module"); return; }
  int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  if (gpsLocked())                   snprintf(out, n, "LOCKED  %d sat", sats);
  else if (gps.charsProcessed())     snprintf(out, n, "acquiring  %d sat", sats);
  else                               snprintf(out, n, "no data");
}
