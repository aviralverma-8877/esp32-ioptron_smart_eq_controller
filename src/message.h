// message.h - a short transient status line shown in the TFT footer.
#pragma once
#include <stdint.h>

extern char     gMsg[30];
extern uint16_t gMsgColor;
extern uint32_t gMsgUntil;      // millis() deadline; 0/past = no message

void setMsg(const char *m, uint16_t color, uint32_t durationMs);
