// xlate.h - optional protocol translator between the Bluetooth client and the
// mount. In XL_NATIVE the bridge stays byte-transparent (iOptron command set,
// for iOptron Commander / ASCOM). In XL_LX200 / XL_NEXSTAR the client speaks
// Meade LX200 or Celestron NexStar - what Stellarium, SkySafari, INDI ... offer
// built in - and every command is answered here, using iOptron 2014 commands
// (:GEC# :Sr :Sd :MS# :CM# :q# ...) on the mount side.
//
// Pure C++ (no Arduino): everything hardware-facing comes in through XlateIo,
// so tools/xlate-test/ can run it against a mock mount on a PC.
#pragma once
#include <stdint.h>
#include <stddef.h>

enum XlateMode : uint8_t { XL_NATIVE = 0, XL_LX200, XL_NEXSTAR, XL_COUNT };

struct XlateIo {
  // iOptron command -> reply (same contract as askMount: `fixedLen` reads exactly
  // N bytes, else up to '#'; timeoutMs 0 = send only). Returns reply length.
  size_t   (*ask)(const char *cmd, char *out, size_t outSz, uint16_t timeoutMs, uint8_t fixedLen);
  void     (*write)(const uint8_t *buf, size_t n);          // to the client
  uint32_t (*millis)();
  bool     (*clock)(long &utcEpoch, long &offsetMin);       // UTC s since 1970, local-UTC min
  bool     (*site)(double &latDeg, double &lonDeg);         // east-positive longitude
  bool     (*altAz)(double raH, double decD, double &altDeg, double &azDeg);   // JNow in
  void     (*log)(const char *fmt, ...);                    // may be null
};

void        xlateBegin(const XlateIo &io);
void        xlateSetMode(XlateMode m);       // also resets the parser state
XlateMode   xlateMode();
const char *xlateModeName(XlateMode m);      // "iOptron", "LX200", "NexStar"

void xlateReset();                           // new client: drop partial commands, target, caches
void xlateFeed(uint8_t c);                   // one byte from the client (LX200 / NexStar modes)
void xlateTick();                            // every loop while connected: timeouts, post-GoTo tracking
