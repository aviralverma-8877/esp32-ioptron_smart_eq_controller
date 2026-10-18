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
  // raw values for GoTo / sky maths (valid flags false until first decode)
  long     localEpoch  = 0;     // :GLT# local time, seconds since 1970-01-01 (as local)
  uint32_t localEpochMs = 0;    // millis() when localEpoch was read
  bool     timeValid   = false;
  double   lonDeg      = 0;     // :Gg#, east-positive
  double   latDeg      = 0;     // :Gt# as reported (may carry a +90 offset, see sky.cpp)
  bool     siteValid   = false;
  int      statusCode  = -1;    // :GAS# digit 1: 0 stopped 1 tracking 2 slewing ... 6 parked
  double   raHours     = 0;     // :GEC# current pointing (JNow), RA in hours
  double   decDegrees  = 0;
  bool     posValid    = false;
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

// ---- GoTo (idle / no BT client only; all block <= ~2 s) ----------------------
enum GotoResult : uint8_t { GOTO_OK, GOTO_NO_LINK, GOTO_BT_BUSY, GOTO_REJECTED, GOTO_COMM_ERR };
// RA/Dec must already be JNow. Sends :ST1# :Sr :Sd :MS# (same sequence as
// iOptron Commander on the SmartEQ Pro). GOTO_REJECTED = :MS# answered '0'
// (below horizon / beyond limits).
GotoResult mountGoto(double raHoursJNow, double decDegJNow);
GotoResult mountGotoZero();                // :MH# - slew to zero (home) position; ends status 7
bool       mountStop();                    // :q#  - stop any slew/move
bool       mountSetTracking(bool on);      // :ST1# / :ST0#
// While true, the idle poll only cycles :GAS# and :GEC# (fast slew feedback).
void       mountSetFastStatus(bool on);

// low-level command/response; `fixedLen` reads exactly N bytes (for :MountInfo#).
size_t askMount(const char *cmd, char *out, size_t outSz,
                uint16_t timeoutMs, uint8_t fixedLen = 0);
