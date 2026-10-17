// ui.h - screen state machine on the 320x240 TFT, driven by the two buttons.
//
//   STATUS  (existing status page, display.cpp)  --A or B-->  MENU   (only when skyReady())
//                                                 hold B   -> manual GPS re-sync
//   MENU    Messier objects / Bright stars / Sync GPS now / Stop mount
//   LIST    scrolling object list with live altitude
//   OBJECT  left box: thumbnail + name;  right box: info text + [ TRACK ] button
//           B = arm TRACK (button turns yellow "CONFIRM"), B again = GoTo + track,
//           any button while slewing = STOP (:q#)
//   hold B anywhere = back one level;  A = next / down, hold A = fast scroll
#pragma once

void uiBegin();     // after displayBegin(), skyBegin(), buttonsBegin()
void uiTick();      // every loop(): consumes button events, redraws as needed
