// mount.h - RS-232 link to the iOptron mount: command I/O, reply decoding,
// idle self-poll, and "is the mount there" link tracking.
#pragma once
#include <Arduino.h>

// Everything the firmware has decoded about the mount, as display-ready strings
// plus the raw offset/DST needed for GPS time maths.
struct MountState {
  char model[6]      = "----";
  char modelName[16] = "";
  char proto[8]      = "";
  char fwMain[10]    = "";
  char fwMotor[10]   = "";
  char utcOffset[8]  = "";      // "+05:30"
  char dst[4]        = "";      // "on" / "off"
  char dateStr[12]   = "";      // "2026-09-07"
  char timeStr[10]   = "";      // "23:17:17"
  char lonStr[14]    = "";      // "77.5667E"
  char latStr[14]    = "";      // "28.6127N"
  char statusStr[12] = "";
  char trackStr[10]  = "";
  char slewRate[6]   = "";
  char guideStr[12]  = "";
  char raStr[12]     = "";
  char decStr[12]    = "";
  char hemi[6]       = "";
  char gpsSrc[8]     = "";      // mount's own time-source flag
  long offsetMin     = 0;       // configured UTC offset, minutes
  int  dstFlag       = 0;
  uint32_t lastOkMs  = 0;
  bool     everOk    = false;
};

extern MountState M;
extern bool       linkUp;        // refreshed by mountUpdateLink()

void   mountBegin();                       // open UART2
void   mountPoll();                        // one self-poll query (idle only)
void   mountRequestTimeRefresh();          // re-poll :GLT.. next (after a sync)
void   mountUpdateLink();                  // recompute `linkUp`
bool   mountLinkUp();

void   mountDrainRx();
void   mountQueryGLT();                    // askMount(":GLT#") + decode

// transparent bridge helpers (client stream <-> mount UART)
void   pumpClientToMount(Stream &client);
void   pumpMountToClient(Print  &client);

// low-level command/response; `fixedLen` reads exactly N bytes (for :MountInfo#).
size_t askMount(const char *cmd, char *out, size_t outSz,
                uint16_t timeoutMs, uint8_t fixedLen = 0);
