// display.h - ST7789 320x240 status screen. Redraws only changed fields.
#pragma once

void displayBegin();
void displayTick();     // throttled internally; call every loop
