// debuglog.h - LOG() sink: UART0 plus a line ring the TFT "Debug log" screen shows.
// Safe from any task (Bluedroid callbacks log too).
#pragma once
#include <stdint.h>

static const int DBG_COLS = 53;          // 320 px / 6 px glyphs
static const int DBG_RING = 96;          // lines kept

void dbgPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void dbgPuts(const char *s);             // same, preformatted

uint32_t dbgSeq();                       // count of finished lines so far
// line `back` lines before the newest (0 = newest); false if not kept anymore
bool dbgLine(int back, char *out, int n);
