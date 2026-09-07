#include "message.h"
#include "config.h"
#include <string.h>

char     gMsg[30]  = "";
uint16_t gMsgColor = C_VAL;
uint32_t gMsgUntil = 0;

void setMsg(const char *m, uint16_t color, uint32_t durationMs) {
  strncpy(gMsg, m, sizeof gMsg - 1);
  gMsg[sizeof gMsg - 1] = 0;
  gMsgColor = color;
  gMsgUntil = millis() + durationMs;
  LOG("[msg] %s\n", m);
}
