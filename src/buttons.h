// buttons.h - two front-panel buttons, sampled by a 10 ms esp_timer so presses
// are never lost while loop() is blocked in a mount query.
//   BTN A = GPIO36 (BTN_NEXT_PIN)   BTN B = GPIO39 (BTN_SYNC_PIN)
//   both active-low with external pull-ups; GPIO34 floats next to them - never read it.
// Single press = navigate (A next, B back), double press A = select.
#pragma once
#include <stdint.h>

enum BtnEvent : uint8_t {
  EV_NONE = 0,
  EV_A,          // A pressed once ("next"); sent DOUBLE_MS after release so it
                 // can't be the first half of a double press
  EV_A_DOUBLE,   // A pressed twice within DOUBLE_MS ("select"); sent on the 2nd press
  EV_A_HOLD,     // A held >= HOLD_MS (fires once; no EV_A follows)
  EV_A_REPEAT,   // A still held: fires every REPEAT_MS after EV_A_HOLD (fast scroll)
  EV_B,          // B pressed ("back"); sent immediately on release
};

void     buttonsBegin();     // configure pins, start the 10 ms sampler
BtnEvent buttonsGet();       // next queued event, EV_NONE if empty (non-blocking)
bool     buttonsAnyDown();   // either button currently held (debounced)
