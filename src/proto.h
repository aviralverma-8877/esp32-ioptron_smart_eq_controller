// proto.h - ESP32 side of the protocol translator (xlate.*): wires it to the
// mount link, GPS / mount clock, site and Bluetooth, and keeps the chosen client
// protocol in NVS ("smarteq"/"proto" - cleared by Factory reset).
#pragma once
#include "xlate.h"

void protoBegin();                  // after mountBegin() / gpsSyncBegin(): load + apply the saved mode
void protoSetMode(XlateMode m);     // apply now (drops any half-read command) and save
void protoCycle();                  // iOptron -> LX200 -> NexStar -> iOptron (UI)
