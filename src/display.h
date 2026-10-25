// display.h - ST7789 320x240 status screen. Redraws only changed fields.
#pragma once

#include <Adafruit_ST7789.h>

extern Adafruit_ST7789 tft;      // shared with ui.cpp / sky.cpp (VSPI, shared with SD)

void displayBegin();
void displayTick();     // status page only; throttled internally
void displayStatusEnter();   // full redraw of the status page (after another screen)
void displayShot(bool delta, uint32_t hz);   // dump the screen on the USB console (see display.cpp)
