#include "debuglog.h"
#include <Arduino.h>
#include <stdarg.h>
#include <string.h>

static char     ring[DBG_RING][DBG_COLS + 1];
static char     cur[DBG_COLS + 1];       // line being assembled (LOG calls may split one)
static int      curN = 0;
static bool     curSkip = false;         // the periodic [state] line stays on UART only
static uint32_t seq = 0;                 // finished lines; newest = ring[(seq - 1) % DBG_RING]
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void dbgPuts(const char *s) {
  Serial.print(s);
  portENTER_CRITICAL(&mux);
  for (; *s; s++) {
    char c = *s;
    if (c == '\r') continue;
    if (c == '\n') {
      if (curN && !curSkip) {
        cur[curN] = 0;
        memcpy(ring[seq % DBG_RING], cur, curN + 1);
        seq++;
      }
      curN = 0; curSkip = false;
      continue;
    }
    if (curN < DBG_COLS) {
      cur[curN++] = (c >= 32 && c < 127) || (uint8_t)c == 0xF8 ? c : '.';
      if (curN == 7 && !memcmp(cur, "[state]", 7)) curSkip = true;
    }
  }
  portEXIT_CRITICAL(&mux);
}

void dbgPrintf(const char *fmt, ...) {
  char b[256];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  dbgPuts(b);
}

uint32_t dbgSeq() { return seq; }

bool dbgLine(int back, char *out, int n) {
  bool ok = false;
  portENTER_CRITICAL(&mux);
  if (back >= 0 && (uint32_t)back < seq && back < DBG_RING) {
    strncpy(out, ring[(seq - 1 - back) % DBG_RING], n - 1);
    out[n - 1] = 0;
    ok = true;
  }
  portEXIT_CRITICAL(&mux);
  return ok;
}
