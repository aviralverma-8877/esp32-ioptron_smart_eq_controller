// buttons.cpp - polled (10 ms esp_timer) debounce. A: single / double press,
// hold + repeat; B: plain press. No GPIO interrupts: GPIO36/39 suffer spurious
// edges (ESP32 erratum 3.11).
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
#define DOUBLE_MS    350        // max gap between A release and the next A press

struct Btn {
  uint8_t  pin;
  bool     down;                // debounced state
  bool     raw;                 // last raw sample
  uint8_t  cnt;                 // consecutive samples equal to `raw`
  bool     held;                // HOLD fired (A) / press to ignore (held at boot)
  bool     pend;                // A: single press waiting for a possible 2nd press
  bool     dbl;                 // A: this press completed a double
  uint32_t downMs, repMs, upMs;
};

static Btn               btn[2] = { { BTN_NEXT_PIN }, { BTN_SYNC_PIN } };
static QueueHandle_t     evQ   = nullptr;
static esp_timer_handle_t tmr  = nullptr;
static volatile bool     anyDown = false;

static void post(BtnEvent e) { if (evQ) xQueueSend(evQ, &e, 0); }

// runs in the esp_timer task (not an ISR) - normal queue API is fine
static void sampleCb(void *) {
  uint32_t now = millis();
  for (int i = 0; i < 2; i++) {
    Btn &b = btn[i];
    bool r = digitalRead(b.pin) == LOW;              // active-low
    if (r == b.raw) { if (b.cnt < 255) b.cnt++; }
    else            { b.raw = r; b.cnt = 1; }

    if (b.cnt >= DEBOUNCE_N && r != b.down) {        // debounced edge
      b.down = r;
      if (i == 1) {                                  // B: back, on release
        if (!r && !b.held) post(EV_B);
        b.held = false;
      } else if (r) {                                // A pressed
        b.downMs = now; b.held = false;
        b.dbl = b.pend && now - b.upMs <= DOUBLE_MS;
        b.pend = false;
        if (b.dbl) post(EV_A_DOUBLE);
      } else {                                       // A released
        if (!b.held && !b.dbl) { b.pend = true; b.upMs = now; }
        b.held = false;
      }
    }
    if (i == 0) {
      if (b.pend && !b.down && now - b.upMs > DOUBLE_MS) { b.pend = false; post(EV_A); }
      if (b.down && !b.dbl) {
        if (!b.held && now - b.downMs >= HOLD_MS) {
          b.held = true; b.repMs = now;
          post(EV_A_HOLD);
        } else if (b.held && now - b.repMs >= REPEAT_MS) {
          b.repMs += REPEAT_MS;
          post(EV_A_REPEAT);
        }
      }
    }
  }
  anyDown = btn[0].down || btn[1].down;
}

void buttonsBegin() {
  for (auto &b : btn) {
    pinMode(b.pin, INPUT);                           // input-only, ext. pull-up
    b.raw = b.down = digitalRead(b.pin) == LOW;      // a button held at boot gives no event
    b.cnt = DEBOUNCE_N; b.held = true;
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
