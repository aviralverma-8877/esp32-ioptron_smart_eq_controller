// buttons.h - three front-panel buttons, sampled by a 10 ms esp_timer so presses
// are never lost while loop() is blocked in a mount query.
//   BTN A = GPIO36 (BTN_A_PIN)    up      } active-low, external pull-ups;
//   BTN B = GPIO39 (BTN_B_PIN)    down    } GPIO34 floats next to them - never read it
//   BOOT  = GPIO0  (BTN_SEL_PIN)  select (short press) / back (long press)
// Holding GPIO0 low during power-on/reset enters the ROM bootloader - harmless,
// but release BOOT before resetting the board.
#pragma once
#include <stdint.h>

enum BtnEvent : uint8_t {
  EV_NONE = 0,
  EV_UP,         // A pressed (sent on press)
  EV_UP_REP,     // A held: every REPEAT_MS after HOLD_MS (fast scroll)
  EV_DOWN,       // B pressed (sent on press)
  EV_DOWN_REP,   // B held: auto-repeat
  EV_SEL,        // BOOT released before HOLD_MS ("select")
  EV_BACK,       // BOOT held >= HOLD_MS (sent once while still held; "back")
};

void     buttonsBegin();     // configure pins, start the 10 ms sampler
BtnEvent buttonsGet();       // next queued event, EV_NONE if empty (non-blocking)
bool     buttonsAnyDown();   // any button currently held (debounced)
void     buttonsInject(BtnEvent e);   // queue an event as if pressed (USB console "btn")
