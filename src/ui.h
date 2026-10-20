// ui.h - screen state machine on the 320x240 TFT, driven by the two buttons.
//
//   STATUS  (existing status page, display.cpp)  --A or B-->  MENU   (only when skyReady())
//   MENU    Messier / Stars / Planets / Sync GPS now / Stop mount / Go to zero /
//           Time zone (BOOT: edit, A/B: +-15 min or Auto, BOOT: save, hold: cancel) /
//           Bluetooth (pair new device for 2 min, client protocol iOptron / LX200 /
//           NexStar, list / forget paired devices) /
//           Factory reset (BOOT twice: forget BT devices, clear settings, reboot)
//   LIST    scrolling object list with live altitude
//   OBJECT  left box: thumbnail + name;  right box: info text + [ TRACK ] button
//           A twice = arm TRACK (button turns yellow "CONFIRM"), A twice again = GoTo,
//           any button while slewing = STOP (:q#)
//   A = next / down, hold A = fast scroll, A twice (double press) = select,
//   B = back one level (or cancel an armed TRACK)
#pragma once

void uiBegin();     // after displayBegin(), skyBegin(), buttonsBegin()
void uiTick();      // every loop(): consumes button events, redraws as needed
