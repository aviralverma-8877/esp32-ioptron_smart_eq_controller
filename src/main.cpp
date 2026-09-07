// ESP32 iOptron SmartEQ controller - top-level wiring only.
// ---------------------------------------------------------------------------
//   BT SPP client <-RFCOMM-> ESP32 UART2 (GPIO17/16) <-> MAX3232 <-RS232-> RJ9 -> mount
//   TFT  (ST7789, HW SPI/VSPI): SCLK18 MOSI23 MISO19  CS25 DC26 RST27
//   GPS  (NEO-6M): TX -> GPIO32 (UART1 RX, receive only)
//   LED2 GPIO2 = mount-link indicator ;  BTN1 GPIO39 = optional manual GPS re-sync
//
//   *** rev 1.0 hw note *** RJ1 pin 3/4 (TXD/RXD) are swapped vs a straight
//   iOptron RJ9 cable - use a cable that swaps pins 3<->4 (pin 1 GND straight).
//
// Modules:  config.h  message  mount  bt_bridge  gps  gps_sync  display
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "config.h"
#include "mount.h"
#include "bt_bridge.h"
#include "gps.h"
#include "gps_sync.h"
#include "display.h"

void setup() {
  pinMode(ACT_LED_PIN, OUTPUT);   digitalWrite(ACT_LED_PIN, LOW);
  pinMode(SD_CS_PIN, OUTPUT);     digitalWrite(SD_CS_PIN, HIGH);   // park shared SPI CS
  pinMode(TOUCH_CS_PIN, OUTPUT);  digitalWrite(TOUCH_CS_PIN, HIGH);

#if defined(DEBUG_LOG) && DEBUG_LOG
  Serial.begin(DEBUG_BAUD);
#endif

  mountBegin();
  gpsBegin();
  gpsSyncBegin();
  displayBegin();
  btBegin();

  LOG("\n[bridge] %s  BT-SPP <-> RS232 @ %d 8N1 (RX=%d TX=%d)  GPS RX=%d  BTN=%d\n",
      BT_NAME, (int)BRIDGE_BAUD, RS232_RX_PIN, RS232_TX_PIN, GPS_RX_PIN, BTN_SYNC_PIN);

  for (int i = 0; i < 3; i++) {
    digitalWrite(ACT_LED_PIN, HIGH); delay(60);
    digitalWrite(ACT_LED_PIN, LOW);  delay(60);
  }
}

#if defined(DEBUG_LOG) && DEBUG_LOG
static void debugState() {
  static uint32_t t = 0;
  if (millis() - t < 3000) return;
  t = millis();
  char gs[32]; gpsStatusStr(gs, sizeof gs);
  LOG("[state] %s(%s) link=%s | %s %s | lon %s lat %s | %s %s | RA %s Dec %s | "
      "GPS %s | BTN=%d | %s\n",
      M.modelName, M.model, linkUp ? "UP" : "down", M.dateStr, M.timeStr,
      M.lonStr, M.latStr, M.statusStr, M.slewRate, M.raStr, M.decStr,
      gs, digitalRead(BTN_SYNC_PIN), btConnected ? "BT-client" : "polling");
}
#endif

void loop() {
  gpsFeed();
  gpsSyncTick();

  if (btConnected) {
    btBridgeTick();                       // transparent, both directions
  } else {
    mountDrainRx();
    static uint32_t pollLastMs = 0;
    if (millis() - pollLastMs >= POLL_PERIOD_MS) { pollLastMs = millis(); mountPoll(); }
  }

  mountUpdateLink();
  digitalWrite(ACT_LED_PIN, linkUp ? HIGH : LOW);   // LED2 = mount link

  displayTick();

#if defined(DEBUG_LOG) && DEBUG_LOG
  debugState();
#endif

  if (!btConnected) delay(2);
}
