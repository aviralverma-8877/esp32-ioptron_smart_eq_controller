// buttons.cpp - polled (10 ms esp_timer) debounce. A / B: press + hold-repeat;
// BOOT: short press = select, long press = back. No GPIO interrupts: GPIO36/39
// suffer spurious edges (ESP32 erratum 3.11).
#include "buttons.h"
#include "config.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#define SAMPLE_MS    10
#define DEBOUNCE_N   3          // equal consecutive samples to accept a change
#define HOLD_MS      600
#define REPEAT_MS    120

enum { B_A, B_B, B_SEL, B_N };

struct Btn {
  uint8_t  pin;
  bool     down;                // debounced state
  bool     raw;                 // last raw sample
  uint8_t  cnt;                 // consecutive samples equal to `raw`
  bool     held;                // HOLD reached (repeat / back fired)
  bool     ign;                 // held since boot: no events until released
  uint32_t downMs, repMs;
};

static Btn               btn[B_N] = { { BTN_A_PIN }, { BTN_B_PIN }, { BTN_SEL_PIN } };
static QueueHandle_t     evQ   = nullptr;
static esp_timer_handle_t tmr  = nullptr;
static volatile bool     anyDown = false;

static void post(BtnEvent e) { if (evQ) xQueueSend(evQ, &e, 0); }

// runs in the esp_timer task (not an ISR) - normal queue API is fine
static void sampleCb(void *) {
  uint32_t now = millis();
  bool any = false;
  for (int i = 0; i < B_N; i++) {
    Btn &b = btn[i];
    bool r = digitalRead(b.pin) == LOW;              // active-low
    if (r == b.raw) { if (b.cnt < 255) b.cnt++; }
    else            { b.raw = r; b.cnt = 1; }

    if (b.cnt >= DEBOUNCE_N && r != b.down) {        // debounced edge
      b.down = r;
      if (r) {                                       // pressed
        b.downMs = now; b.held = false;
        if (i == B_A) post(EV_UP);
        if (i == B_B) post(EV_DOWN);
      } else {                                       // released
        if (i == B_SEL && !b.held && !b.ign) post(EV_SEL);
        b.held = b.ign = false;
      }
    }
    if (b.ign) { any |= b.down; continue; }
    if (b.down && !b.held && now - b.downMs >= HOLD_MS) {
      b.held = true; b.repMs = now;
      if (i == B_SEL) post(EV_BACK);
      else            post(i == B_A ? EV_UP_REP : EV_DOWN_REP);
    } else if (b.down && b.held && i != B_SEL && now - b.repMs >= REPEAT_MS) {
      b.repMs += REPEAT_MS;
      post(i == B_A ? EV_UP_REP : EV_DOWN_REP);
    }
    any |= b.down;
  }
  anyDown = any;
}

void buttonsBegin() {
  pinMode(BTN_A_PIN, INPUT);                         // input-only, ext. pull-up
  pinMode(BTN_B_PIN, INPUT);
  pinMode(BTN_SEL_PIN, INPUT_PULLUP);                // BOOT: board pull-up + internal
  for (auto &b : btn) {
    b.raw = b.down = digitalRead(b.pin) == LOW;      // a button held at boot gives no event
    b.cnt = DEBOUNCE_N; b.held = false; b.ign = b.down;
  }
  evQ = xQueueCreate(16, sizeof(BtnEvent));
  esp_timer_create_args_t a = {};
  a.callback = sampleCb;
  a.name     = "btn";
  esp_timer_create(&a, &tmr);
  esp_timer_start_periodic(tmr, SAMPLE_MS * 1000);
}

BtnEvent buttonsGet() {
  BtnEvent e = EV_NONE;
  if (evQ && xQueueReceive(evQ, &e, 0) == pdTRUE) return e;
  return EV_NONE;
}

bool buttonsAnyDown() { return anyDown; }
