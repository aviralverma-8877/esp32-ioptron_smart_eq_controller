// buttons.h - two front-panel buttons, sampled by a 10 ms esp_timer so presses
// are never lost while loop() is blocked in a mount query.
//   BTN A = GPIO36 (BTN_NEXT_PIN)   BTN B = GPIO39 (BTN_SYNC_PIN)
//   both active-low with external pull-ups; GPIO34 floats next to them - never read it.
#pragma once
#include <stdint.h>

enum BtnEvent : uint8_t {
  EV_NONE = 0,
  EV_A,          // A pressed and released before HOLD_MS ("next")
  EV_A_HOLD,     // A held >= HOLD_MS (fires once)
  EV_A_REPEAT,   // A still held: fires every REPEAT_MS after EV_A_HOLD (fast scroll)
  EV_B,          // B short press ("select / OK")
  EV_B_HOLD,     // B held >= HOLD_MS (fires once, "back"); no short event follows
};

void     buttonsBegin();     // configure pins, start the 10 ms sampler
BtnEvent buttonsGet();       // next queued event, EV_NONE if empty (non-blocking)
bool     buttonsAnyDown();   // either button currently held (debounced)
