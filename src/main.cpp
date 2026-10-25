// ESP32 iOptron SmartEQ controller - top-level wiring only.
// ---------------------------------------------------------------------------
//   BT SPP client <-RFCOMM-> ESP32 UART2 (GPIO17/16) <-> MAX3232 <-RS232-> RJ9 -> mount
//   TFT  (ST7789, HW SPI/VSPI): SCLK18 MOSI23 MISO19  CS25 DC26 RST27
//   GPS  (NEO-6M): TX -> GPIO32 (UART1 RX, receive only)
//   LED2 GPIO2 = mount-link indicator ;  BTN A GPIO36 up / B GPIO39 down / BOOT GPIO0 select = UI
//   SD card on the LCD module (CS GPIO33, shared VSPI) = sky database + GoTo
//
// Modules:  config.h  message  mount  bt_bridge  gps  gps_sync  display
//           buttons  sky  ui  xlate (LX200 / NexStar translator) + proto (its glue)
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "config.h"
#include "mount.h"
#include "bt_bridge.h"
#include "gps.h"
#include "gps_sync.h"
#include "display.h"
#include "buttons.h"
#include "sky.h"
#include "ui.h"
#include "proto.h"

void setup() {
  pinMode(ACT_LED_PIN, OUTPUT);   digitalWrite(ACT_LED_PIN, LOW);
  pinMode(SD_CS_PIN, OUTPUT);     digitalWrite(SD_CS_PIN, HIGH);   // park shared SPI CS
  pinMode(TOUCH_CS_PIN, OUTPUT);  digitalWrite(TOUCH_CS_PIN, HIGH);

#if defined(DEBUG_LOG) && DEBUG_LOG
  Serial.begin(DEBUG_BAUD);
#endif

  btBegin();                      // first: Bluedroid needs a big heap before SD/sky allocate
  mountBegin();
  gpsBegin();
  gpsSyncBegin();
  protoBegin();                   // BT client protocol (iOptron / LX200 / NexStar), NVS
  displayBegin();
  skyBegin();
  buttonsBegin();
  uiBegin();

  LOG("\n[bridge] %s  BT-SPP <-> RS232 @ %d 8N1 (RX=%d TX=%d)  GPS RX=%d  BTN A=%d B=%d SEL=%d\n",
      BT_NAME, (int)BRIDGE_BAUD, RS232_RX_PIN, RS232_TX_PIN, GPS_RX_PIN,
      BTN_A_PIN, BTN_B_PIN, BTN_SEL_PIN);

  for (int i = 0; i < 3; i++) {
    digitalWrite(ACT_LED_PIN, HIGH); delay(60);
    digitalWrite(ACT_LED_PIN, LOW);  delay(60);
  }
}

#if defined(DEBUG_LOG) && DEBUG_LOG
static bool stateLog = true;

static void listBonds() {
  int n = btBondCount();
  LOG("[cons] %d paired device(s)\n", n);
  for (int i = 0; i < n; i++) {
    uint8_t m[6]; char nm[24];
    if (btBondGet(i, m, nm, sizeof nm))
      LOG("[cons]  %d: %02X:%02X:%02X:%02X:%02X:%02X %s\n", i, m[0], m[1], m[2], m[3], m[4], m[5], nm);
  }
}

// USB debug console: a line starting with ':' (e.g. ":GAC#") goes to the mount
// as-is and the reply is printed; "." toggles the [state] line. Idle only.
// Every command is echoed into the log and brings up the TFT "Debug log"
// screen, so what the console does is visible on the device ("tft" leaves it).
static void debugConsole() {
  static char line[48];
  static uint8_t n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c != '\r' && c != '\n') { if (n < sizeof line - 1) line[n++] = c; continue; }
    if (!n) continue;
    line[n] = 0; n = 0;
    if (!strcmp(line, "tft")) { uiShowDebug(!uiDebugShown()); continue; }
    // screen-neutral commands: no echo, no switch to the log screen (filming the UI)
    if (!strncmp(line, "shot", 4) && (!line[4] || line[4] == ' ' || line[4] == 'd')) {
      bool d = line[4] == 'd';                                         // shotd: changed rows only
      long hz = atol(line + 4 + d);                                    // optional SPI read clock
      displayShot(d, hz > 0 ? hz : 10000000);
      continue;
    }
    if (!strncmp(line, "baud ", 5)) {                                  // e.g. baud 921600
      long b = atol(line + 5);
      if (b >= 9600) { Serial.flush(); Serial.updateBaudRate(b); }
      continue;
    }
    LOG("> %s\n", line);
    if (!strncmp(line, "btn ", 4)) {                                   // drives the current screen
      static const struct { const char *k; BtnEvent e; } keys[] = {
        { "a", EV_UP }, { "b", EV_DOWN }, { "sel", EV_SEL }, { "back", EV_BACK },
        { "a+", EV_UP_REP }, { "b+", EV_DOWN_REP } };
      bool ok = false;
      for (auto &k : keys) if (!strcmp(line + 4, k.k)) { buttonsInject(k.e); ok = true; }
      if (!ok) LOG("[cons] btn a|b|sel|back|a+|b+  (A up, B down, BOOT short/hold, held A/B)\n");
      continue;
    }
    uiShowDebug(true);
    if (!strcmp(line, ".")) { stateLog = !stateLog; continue; }
    if (!strcmp(line, "bonds"))  { listBonds(); continue; }
    if (!strncmp(line, "forget", 6)) {                                 // forget <n> | forget all
      const char *a = line + 6;
      while (*a == ' ') a++;
      if (!strcmp(a, "all")) btBondForgetAll();
      else if (*a >= '0' && *a <= '9') { if (!btBondForget(atoi(a))) LOG("[cons] no device %s\n", a); }
      else LOG("[cons] forget <n|all> - see 'bonds'\n");
      listBonds();
      continue;
    }
    if (!strcmp(line, "trace")) {                                      // log client commands + replies
      xlateSetTrace(!xlateTrace());
      LOG("[cons] translator trace %s\n", xlateTrace() ? "ON" : "off");
      continue;
    }
    if (!strncmp(line, "proto", 5)) {                                  // proto [ioptron|lx200|nexstar]
      const char *a = line + 5;
      while (*a == ' ') a++;
      for (int m = 0; *a && m < XL_COUNT; m++)
        if (!strcasecmp(a, xlateModeName((XlateMode)m))) protoSetMode((XlateMode)m);
      LOG("[cons] BT client protocol: %s\n", xlateModeName(xlateMode()));
      continue;
    }
    if (line[0] != ':') {
      LOG("[cons] ':cmd#' -> mount, '.' [state] on/off, proto [ioptron|lx200|nexstar], trace\n");
      LOG("[cons] bonds, forget <n|all>, tft (log screen)\n");
      LOG("[cons] btn a|b|sel|back|a+|b+ (press), shot / shotd (screen dump, delta), baud <n>\n");
      continue;
    }
    if (btConnected) { LOG("[cons] BT client active - not sent\n"); continue; }
    char r[64];
    size_t got = askMount(line, r, sizeof r, 1000);
    LOG("[cons] %s -> %s\n", line, got ? r : "(no reply)");
  }
}

static void debugState() {
  static uint32_t t = 0;
  if (!stateLog || millis() - t < 3000) return;
  t = millis();
  char gs[32]; gpsStatusStr(gs, sizeof gs);
  LOG("[state] %s(%s) link=%s | %s %s | lon %s lat %s hemi %s | %s %s | RA %s Dec %s | "
      "GPS %s | SD %s | %s\n",
      M.modelName, M.model, linkUp ? "UP" : "down", M.dateStr, M.timeStr,
      M.lonStr, M.latStr, M.hemi, M.statusStr, M.slewRate, M.raStr, M.decStr,
      gs, skyReady() ? "ok" : "-", btConnected ? "BT-client" : "polling");
}
#endif

void loop() {
  gpsFeed();
  gpsSyncTick();
  btTick();

  if (btConnected) {
    btBridgeTick();                       // transparent, both directions
  } else {
    mountDrainRx();
    static uint32_t pollLastMs = 0;
    if (millis() - pollLastMs >= POLL_PERIOD_MS) { pollLastMs = millis(); mountPoll(); }
  }

  mountUpdateLink();
  digitalWrite(ACT_LED_PIN, linkUp ? HIGH : LOW);   // LED2 = mount link

  skyTick();
  uiTick();                               // status page or catalog UI

#if defined(DEBUG_LOG) && DEBUG_LOG
  debugConsole();
  debugState();
#endif

  if (!btConnected) delay(2);
}
