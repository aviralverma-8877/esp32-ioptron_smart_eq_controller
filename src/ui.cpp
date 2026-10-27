// ui.cpp - screen state machine: status page -> catalog menu -> object list ->
// object detail with a TRACK (GoTo) button. Three buttons: A = up, B = down
// (hold = fast scroll), BOOT = select, hold BOOT = back. Full clears only on screen changes; otherwise only the changed
// rows / fields are repainted (text drawn with a background colour, no flicker).
//
// MENU                                   LIST (9 rows x 22 px)
// +--------------------------------+     +--------------------------------+
// | Sky catalog                    |     | Messier  M31            27/110 |
// |================================|     |================================|
// |#Messier objects (110)##########|     | M30  Jellyfish Cluster     12d |
// | Bright stars (60)              |     |#M31  Andromeda Galaxy ####45d##| <- C_SEL
// | Sync GPS now                   |     | M32  Le Gentil            -8d  | <- dimmed
// | Stop mount                     |     |  ...                           |
// | Back                           |     |--------------------------------|
// |--------------------------------|     | A/B:up/dn  BOOT:open  hold:menu|
// | A/B:up/dn  BOOT:sel  hold:back |     +--------------------------------+
// +--------------------------------+
//
// OBJECT
// +------------+ +-------------------------------------+
// | +--------+ | | Object info                         |
// | | 96x96  | | |-------------------------------------|
// | | thumb  | | | Galaxy  And  mag 3.4  178'          |
// | +--------+ | |-------------------------------------|
// | M31        | | description, word-wrapped, 33 col   |
// | Andromeda  | | ...                                 |
// | Galaxy     | | Scope: ...                          |
// | Galaxy     | | Camera: ...                         |
// +------------+ | Exposure: ...                       |
//  Alt 45d Az120 | +--------------+                    |
//  RA 00h42m     | |    TRACK     |  RA 00h42m51s      | <- live mount pos /
//  Dec +41d16'   | +--------------+  Dec +41d16'       |    "below 10d" note
//  A/B: prev/next+-------------------------------------+
//  hold BOOT:back
//
// TRACK button: TRACK (disabled/grey below MIN_TRACK_ALT_DEG) -SEL-> CONFIRM? -SEL->
// GoTo -> SLEWING -> TRACKING. Any button while SLEWING = stop (:q#), acted on
// the moment it goes down.
#include "ui.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "sky.h"
#include "mount.h"
#include "gps_sync.h"
#include "message.h"
#include "bt_bridge.h"
#include "proto.h"
#include "xlate.h"
#include "debuglog.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define MIN_TRACK_ALT_DEG 10.0         // TRACK enabled only at/above this altitude
#define MIN_SUN_SEP_DEG   15.0         // ... and never this close to the Sun (safety)
#define DEG "\xF8"                     // CP437 degree sign (tft.cp437(true))

static const int SCR_W = 320, SCR_H = 240;
static const uint32_t CONFIRM_MS = 5000, RESULT_MS = 3000, FLASH_MS = 1500;
static const uint32_t SLEW_MAX_MS = 240000UL, ALT_OBJ_MS = 5000, ALT_LIST_MS = 20000;
static const uint32_t STOP_IGNORE_MS = 800;     // drop the events of the press that stopped a slew
static uint32_t ignoreUntil = 0;

enum Screen { SC_STATUS, SC_MENU, SC_LIST, SC_OBJECT, SC_BT, SC_DEBUG };
static Screen scr = SC_STATUS;

// ---- drawing helpers ---------------------------------------------------------
static void txt(int x, int y, const char *s, uint16_t fg, uint8_t sz, uint16_t bg = BG) {
  tft.setTextSize(sz); tft.setTextColor(fg, bg); tft.setCursor(x, y); tft.print(s);
}

static void titleBar(const char *t) {           // clears the screen
  tft.fillScreen(BG);
  txt(4, 5, t, C_HEAD, 2);
  tft.drawFastHLine(0, 26, SCR_W, C_HEAD);
}

static void footHint(const char *h) {
  tft.drawFastHLine(0, 227, SCR_W, C_SEP);
  txt(4, 231, h, C_LBL, 1);
}

static uint16_t altColor(bool ok, double alt) {
  return !ok ? C_LBL : alt >= MIN_TRACK_ALT_DEG ? C_OK : alt >= 0 ? C_WARN : C_BAD;
}

static void fmtAlt(bool ok, double alt, char *b, size_t n) {
  if (ok) snprintf(b, n, "%d" DEG, (int)lround(alt)); else snprintf(b, n, "--");
}

static void fmtRA(double h, char *b, size_t n, bool sec) {
  long s = lround(fmod(h, 24.0) * 3600.0);
  if (s < 0) s += 86400;
  s %= 86400;
  if (sec) snprintf(b, n, "RA %02ldh%02ldm%02lds", s / 3600, s / 60 % 60, s % 60);
  else     snprintf(b, n, "RA %02ldh%02ldm", s / 3600, s / 60 % 60);
}

static void fmtDec(double d, char *b, size_t n) {
  long m = lround(fabs(d) * 60.0);
  snprintf(b, n, "Dec %c%02ld" DEG "%02ld'", d < 0 ? '-' : '+', m / 60, m % 60);
}

// Cut one word-wrapped line (<= cols chars) off the front of p.
static void takeLine(const char *&p, int cols, char *out) {
  while (*p == ' ') p++;
  int n = strlen(p);
  if (n > cols) {
    n = cols;                                     // long word: hard break
    for (int i = cols; i > 0; i--) if (p[i] == ' ') { n = i; break; }
  }
  int k = n;
  while (k > 0 && p[k - 1] == ' ') k--;
  memcpy(out, p, k); out[k] = 0;
  p += n;
}

// Size-1 word-wrapped text, 10 px pitch, glyphs kept above maxY. The first line
// starts `skip` chars in (after a label). The last line that fits ends in "..."
// if text remains. Returns the y of the next line.
static int wrapDraw(int x, int y, int maxY, const char *s, int cols, uint16_t fg, int skip = 0) {
  char ln[64];
  const char *p = s;
  if (cols > 60) cols = 60;
  while (*p == ' ') p++;
  while (*p && y + 8 <= maxY) {
    int c = cols - skip;
    if (c < 4) c = 4;
    takeLine(p, c, ln);
    while (*p == ' ') p++;
    if (*p && y + 18 > maxY) {                    // out of room: mark the cut
      int n = strlen(ln);
      if (n > c - 3) n = c - 3;
      strcpy(ln + n, "...");
    }
    txt(x + skip * 6, y, ln, fg, 1);
    y += 10; skip = 0;
  }
  return y;
}

static void manualSync() {
  bool held = btConnected && xlateMode() == XL_NATIVE;   // transparent bridge in use
  gpsSyncRequestManual();
  setMsg(held ? "BT busy - sync deferred" : "GPS sync requested", held ? C_WARN : C_HEAD, 3000);
}

// ---- STATUS -------------------------------------------------------------------
static void statusEnter() { scr = SC_STATUS; displayStatusEnter(); }

static void menuEnter();

static void statusEvent(BtnEvent e) {
  if (e == EV_UP || e == EV_DOWN || e == EV_SEL) menuEnter();   // catalog rows need the SD
}

// ---- MENU ---------------------------------------------------------------------
static const int MENU_N = 11, MENU_Y = 30, MENU_H = 17;
static const int MENU_ZERO = 5;                   // "Go to zero position" row
static const int MENU_TZ = 6;                     // "Time zone" row (edited in place)
static const int MENU_BT = 7;                     // "Bluetooth" -> pairing screen
static const int MENU_RESET = 8;                  // "Factory reset" (BOOT twice)
static const int MENU_DEBUG = 9;                  // "Debug log" -> live log screen
static bool     resetArmed = false;
static uint32_t resetArmMs = 0;
static int menuSel = 0;
static bool tzEdit = false;                       // TZ row: A/B change, BOOT save, hold BOOT cancel
static int  tzVal  = TZ_AUTO;

static void fmtTz(int m, char *b, size_t n) {
  snprintf(b, n, "UTC%c%02d:%02d", m < 0 ? '-' : '+', abs(m) / 60, abs(m) % 60);
}
static bool     zeroArmed = false;                // zero row awaiting its confirm press
static uint32_t zeroArmMs = 0;
static bool     zeroSlew = false, zeroSeenSlew = false;   // :MH# slew in progress
static uint32_t zeroMs = 0;

static void menuRow(int i) {
  char b[32];
  bool sel = i == menuSel;
  uint16_t bg = sel ? C_SEL : BG, fg = C_VAL;
  int y = MENU_Y + i * MENU_H;
  switch (i) {
    case 0:  snprintf(b, sizeof b, "Messier objects (%d)", skyCount(CAT_MESSIER)); break;
    case 1:  snprintf(b, sizeof b, "Bright stars (%d)", skyCount(CAT_STAR)); break;
    case 2:  snprintf(b, sizeof b, "Planets & Moon (%d)", skyCount(CAT_PLANET)); break;
    case 3:  snprintf(b, sizeof b, "Sync GPS now"); break;
    case 4:  snprintf(b, sizeof b, "Stop mount"); break;
    case MENU_ZERO:
      if (zeroArmed) { snprintf(b, sizeof b, "Zero: BOOT again=GO"); fg = C_WARN; }
      else             snprintf(b, sizeof b, "Go to zero position");
      break;
    case MENU_TZ: {
      char z[12];
      int v = tzEdit ? tzVal : gpsSyncTzSetting();
      fmtTz(v == TZ_AUTO ? gpsSyncTzAuto() : v, z, sizeof z);
      if (tzEdit) { snprintf(b, sizeof b, "TZ < %s%s >", v == TZ_AUTO ? "Auto " : "", z); fg = C_WARN; }
      else          snprintf(b, sizeof b, "Time zone %s%s", v == TZ_AUTO ? "A " : "", z);
      break;
    }
    case MENU_BT: snprintf(b, sizeof b, "Bluetooth (%d paired)", btBondCount()); break;
    case MENU_RESET:
      if (resetArmed) { snprintf(b, sizeof b, "Reset: BOOT again=ERASE"); fg = C_BAD; }
      else              snprintf(b, sizeof b, "Factory reset");
      break;
    case MENU_DEBUG: snprintf(b, sizeof b, "Debug log"); break;
    default: snprintf(b, sizeof b, "Back"); break;
  }
  tft.fillRect(0, y, SCR_W, MENU_H - 1, bg);
  if (sel) tft.fillRect(0, y, 4, MENU_H - 1, C_HEAD);     // focus bar
  txt(14, y, b, fg, 2, bg);
}

static void zeroDisarm() {
  if (!zeroArmed) return;
  zeroArmed = false;
  if (scr == SC_MENU) menuRow(MENU_ZERO);
}

static void resetDisarm() {
  if (!resetArmed) return;
  resetArmed = false;
  if (scr == SC_MENU) menuRow(MENU_RESET);
}

// Forget every paired Bluetooth device, wipe our saved settings, reboot.
// The mount's own settings (site, time zone) are not touched.
static void factoryReset() {
  LOG("[ui] factory reset\n");
  titleBar("Factory reset");
  txt(14, 60, "Erasing settings and", C_VAL, 2);
  txt(14, 84, "paired devices...", C_VAL, 2);
  btBondForgetAll();
  gpsSyncFactoryReset();
  txt(14, 124, "Restarting", C_WARN, 2);
  delay(1500);
  ESP.restart();
}

static void doGotoZero() {
  switch (mountGotoZero()) {
    case GOTO_OK:
      zeroSlew = true; zeroSeenSlew = false; zeroMs = millis();
      mountSetFastStatus(true);
      setMsg("Slewing to zero - any button stops", C_WARN, 5000);
      break;
    case GOTO_BT_BUSY: setMsg("BT busy - not moved", C_WARN, 3000); break;
    case GOTO_NO_LINK: setMsg("No mount link", C_BAD, 3000); break;
    default:           setMsg("Zero slew rejected", C_BAD, 3000); break;
  }
}

static void zeroStop(const char *why, uint16_t color) {
  mountStop();
  mountSetFastStatus(false);
  zeroSlew = false;
  setMsg(why, color, 4000);
}

// Runs every pass while a zero slew is active (any screen).
static void zeroTick() {
  if (M.statusCode == 2) zeroSeenSlew = true;
  uint32_t el = millis() - zeroMs;
  if (el > SLEW_MAX_MS) zeroStop("Zero slew TIMEOUT", C_BAD);
  else if ((zeroSeenSlew || el > 3000) && M.statusCode != 2) {
    mountSetFastStatus(false);
    zeroSlew = false;
    if (M.statusCode == 7) setMsg("At zero position", C_OK, 4000);
    else                   setMsg("Zero slew ended", C_WARN, 4000);
  }
}

static void menuEnter() {
  scr = SC_MENU; tzEdit = false;
  titleBar("Sky catalog");
  for (int i = 0; i < MENU_N; i++) menuRow(i);
  footHint("A:up  B:down  BOOT:select  hold BOOT:back");
}

static void listEnter(SkyCat c);
static void btEnter();
static void dbgEnter(Screen back);

// TZ edit: steps of 15 min between UTC-12 and UTC+14, "Auto" past either end
static void tzEvent(BtnEvent e) {
  switch (e) {
    case EV_UP: case EV_UP_REP: case EV_DOWN: case EV_DOWN_REP: {
      int d = (e == EV_UP || e == EV_UP_REP) ? 15 : -15;
      if (tzVal == TZ_AUTO) tzVal = gpsSyncTzAuto();   // leave Auto at its own value
      else {
        tzVal += d;
        if (tzVal > TZ_MAX || tzVal < TZ_MIN) tzVal = TZ_AUTO;
      }
      break;
    }
    case EV_SEL:
      tzEdit = false;
      gpsSyncTzSet(tzVal);
      setMsg("Time zone saved - syncing", C_OK, 3000);
      manualSync();
      break;
    case EV_BACK: tzEdit = false; break;
    default: break;
  }
  menuRow(MENU_TZ);
}

static void menuEvent(BtnEvent e) {
  if (tzEdit) { tzEvent(e); return; }
  if (zeroArmed && !(e == EV_SEL && menuSel == MENU_ZERO)) zeroDisarm();
  if (resetArmed && !(e == EV_SEL && menuSel == MENU_RESET)) resetDisarm();
  switch (e) {
    case EV_UP: case EV_UP_REP: case EV_DOWN: case EV_DOWN_REP: {
      int o = menuSel, d = (e == EV_UP || e == EV_UP_REP) ? -1 : 1;
      menuSel = (menuSel + d + MENU_N) % MENU_N;
      menuRow(o); menuRow(menuSel);
      break;
    }
    case EV_SEL:
      switch (menuSel) {
        case 0: case 1: case 2: {
          SkyCat c = menuSel == 0 ? CAT_MESSIER : menuSel == 1 ? CAT_STAR : CAT_PLANET;
          if (skyCount(c) > 0) listEnter(c);
          else setMsg(SKY_NO_CAT_MSG, C_WARN, 2500);
          break;
        }
        case 3: manualSync(); statusEnter(); break;
        case 4:
          if (btConnected) setMsg("BT busy - not stopped", C_WARN, 3000);
          else {
            bool ok = mountStop();
            mountSetFastStatus(false);
            zeroSlew = false;
            setMsg(ok ? "Mount stopped" : "Stop failed (no mount?)", ok ? C_OK : C_BAD, 3000);
          }
          statusEnter();
          break;
        case MENU_ZERO:
          if (!zeroArmed) {                     // first select only arms it
            zeroArmed = true; zeroArmMs = millis();
            menuRow(MENU_ZERO);
          } else {
            zeroArmed = false;
            doGotoZero();
            statusEnter();
          }
          break;
        case MENU_TZ:
          tzEdit = true; tzVal = gpsSyncTzSetting();
          menuRow(MENU_TZ);
          break;
        case MENU_BT: btEnter(); break;
        case MENU_DEBUG: dbgEnter(SC_MENU); break;
        case MENU_RESET:
          if (!resetArmed) { resetArmed = true; resetArmMs = millis(); menuRow(MENU_RESET); }
          else factoryReset();
          break;
        default: statusEnter(); break;
      }
      break;
    case EV_BACK: statusEnter(); break;
    default: break;
  }
}

// ---- LIST ---------------------------------------------------------------------
static const int LIST_Y = 28, ROW_H = 22, ROWS = 9;
static const int ALT_W = 4 * 12;                  // "-45deg" column, size 2
static SkyCat listCat = CAT_MESSIER;
static int    listSel[CAT_COUNT] = {};
static int    listTop = 0;
static char   rowAlt[ROWS][8];                    // altitude text shown per visible row
static uint32_t listAltMs = 0;

static void listRow(int r) {
  int i = listTop + r, y = LIST_Y + r * ROW_H;
  bool sel = i == listSel[listCat];
  uint16_t bg = sel ? C_SEL : BG;
  tft.fillRect(0, y, SCR_W, ROW_H, bg);
  rowAlt[r][0] = 0;
  const SkyObj *o = skyGet(listCat, i);
  if (!o) return;
  if (sel) tft.fillRect(0, y, 3, ROW_H, C_HEAD);
  double alt = 0, az = 0;
  bool ok = skyAltAz(o, alt, az), below = ok && alt < 0;
  char b[40];
  snprintf(b, sizeof b, "%.10s", o->label);
  int nx = 8 + (int)strlen(b) * 12 + 8;
  txt(8, y + 3, b, below ? C_LBL : C_VAL, 2, bg);
  int nmax = (SCR_W - 6 - ALT_W - 6 - nx) / 6;
  if (nmax > 0) {
    snprintf(b, sizeof b, "%.*s", nmax < 39 ? nmax : 39, o->name);
    txt(nx, y + 7, b, below ? C_LBL : C_HEAD, 1, bg);
  }
  fmtAlt(ok, alt, rowAlt[r], sizeof rowAlt[r]);
  txt(SCR_W - 6 - (int)strlen(rowAlt[r]) * 12, y + 3, rowAlt[r], altColor(ok, alt), 2, bg);
}

static void listTitle() {
  int n = skyCount(listCat), s = listSel[listCat];
  const SkyObj *o = skyGet(listCat, s);
  char b[16], p[12];
  snprintf(b, sizeof b, "%-11.11s", o ? o->label : "");
  txt(96, 5, b, C_VAL, 2);
  snprintf(p, sizeof p, "%d/%d", s + 1, n);
  snprintf(b, sizeof b, "%7s", p);
  txt(SCR_W - 4 - 7 * 12, 5, b, C_LBL, 2);
}

static void listEnter(SkyCat c) {
  scr = SC_LIST; listCat = c;
  int n = skyCount(c);
  if (listSel[c] >= n || listSel[c] < 0) listSel[c] = 0;
  listTop = listSel[c] / ROWS * ROWS;
  titleBar(c == CAT_MESSIER ? "Messier" : c == CAT_STAR ? "Stars" : "Planets");
  for (int r = 0; r < ROWS; r++) listRow(r);
  listTitle();
  footHint("A:up  B:down  hold:fast  BOOT:open  hold BOOT:menu");
  listAltMs = millis();
}

static void listMove(int ns) {
  int n = skyCount(listCat);
  if (n <= 0) return;
  ns = (ns % n + n) % n;
  int old = listSel[listCat];
  listSel[listCat] = ns;
  int top = ns / ROWS * ROWS;
  if (top != listTop) {                           // new page: repaint all rows
    listTop = top;
    for (int r = 0; r < ROWS; r++) listRow(r);
  } else {                                        // same page: old + new row only
    listRow(old - listTop);
    listRow(ns - listTop);
  }
  listTitle();
}

static void listRefreshAlt() {                    // objects rise/set: repaint changed rows
  listAltMs = millis();
  char b[8];
  for (int r = 0; r < ROWS; r++) {
    const SkyObj *o = skyGet(listCat, listTop + r);
    if (!o) continue;
    double alt, az;
    bool ok = skyAltAz(o, alt, az);
    fmtAlt(ok, alt, b, sizeof b);
    if (strcmp(b, rowAlt[r])) listRow(r);
  }
}

static void objEnter(int i);

static void listEvent(BtnEvent e) {
  switch (e) {
    case EV_UP:   case EV_UP_REP:   listMove(listSel[listCat] - 1); break;
    case EV_DOWN: case EV_DOWN_REP: listMove(listSel[listCat] + 1); break;
    case EV_SEL:  objEnter(listSel[listCat]); break;
    case EV_BACK: menuEnter(); break;
    default: break;
  }
}

// ---- OBJECT -------------------------------------------------------------------
static const int LB_W = 104, LB_H = 166;                  // left box (thumb + name)
static const int RB_X = 108, RB_W = SCR_W - RB_X;         // right box, full height
static const int TX_X = 114, TX_COLS = 33, TX_Y = 42, TX_MAXY = 200;
static const int BT_X = 112, BT_Y = 206, BT_W = 126, BT_H = 28;
static const int SD_X = 244;                              // live text beside the button

enum BtnState { BS_READY, BS_CONFIRM, BS_SLEW, BS_TRACK, BS_MSG };
static const SkyObj *obj = nullptr;
static int8_t   trackCat = -1;                  // object the last GoTo ended tracking on
static int      trackIdx = -1;

// :GAS# 1 tracking, 3 guiding, 4 meridian flip, 5 tracking + PEC
static bool mountTracking() {
  int s = M.statusCode;
  return s == 1 || s == 3 || s == 4 || s == 5;
}
static BtnState bs = BS_READY;
static uint32_t bsMs = 0, flashUntil = 0, altMs = 0, drawMs = 0;
static const char *msgLbl = "";                 // BS_MSG: label / colour / fill / duration
static uint16_t msgCol = C_BAD;
static bool     msgFill = false, seenSlew = false, altOk = false, sunNear = false;
static uint32_t msgDur = RESULT_MS;
static double   oAlt = 0, oAz = 0;
static char     btnCache[24], sideCache[40], liveCache[32], hintCache[40];
static char     info[4][200];

static bool trackEnabled() { return altOk && oAlt >= MIN_TRACK_ALT_DEG && !sunNear; }
static void bsSet(BtnState s) { bs = s; bsMs = millis(); }
static void bsMsg(const char *l, uint16_t c, bool fill, uint32_t dur) {
  msgLbl = l; msgCol = c; msgFill = fill; msgDur = dur; bsSet(BS_MSG);
}

static void calcAlt() {
  altOk = obj && skyAltAz(obj, oAlt, oAz);
  double sep;                                     // unknown separation = treat as unsafe
  sunNear = !(obj && skySunSep(obj, sep)) || sep < MIN_SUN_SEP_DEG;
  altMs = millis();
}

static void drawButton() {
  const char *l; uint16_t c; bool fill = false;
  switch (bs) {
    case BS_CONFIRM: l = "CONFIRM?"; c = C_WARN; fill = true; break;
    case BS_SLEW:    l = "SLEWING";  c = C_WARN; break;
    case BS_TRACK:   l = "TRACKING"; c = C_OK;   fill = true; break;
    case BS_MSG:     l = msgLbl;     c = msgCol; fill = msgFill; break;
    default:         l = "TRACK";    c = trackEnabled() ? C_HEAD : C_LBL; break;
  }
  char key[24];
  snprintf(key, sizeof key, "%s|%04x|%d", l, (unsigned)c, (int)fill);
  if (!strcmp(key, btnCache)) return;
  strncpy(btnCache, key, sizeof btnCache - 1);
  uint16_t bg = fill ? c : BG, fg = fill ? (c == C_BAD ? C_VAL : BG) : c;
  tft.fillRoundRect(BT_X, BT_Y, BT_W, BT_H, 6, bg);
  if (!fill) {
    tft.drawRoundRect(BT_X, BT_Y, BT_W, BT_H, 6, c);
    tft.drawRoundRect(BT_X + 1, BT_Y + 1, BT_W - 2, BT_H - 2, 5, c);
  }
  txt(BT_X + (BT_W - (int)strlen(l) * 12) / 2, BT_Y + 6, l, fg, 2, bg);
}

static void drawSide() {                         // 2 lines x 12 chars beside the button
  char a[16] = "", b[16] = "", key[40];
  uint16_t c = C_LBL;
  if (bs == BS_SLEW || bs == BS_TRACK) {
    c = bs == BS_SLEW ? C_WARN : C_VAL;
    if (M.posValid) { fmtRA(M.raHours, a, sizeof a, true); fmtDec(M.decDegrees, b, sizeof b); }
    else snprintf(a, sizeof a, "RA --");
  } else if (bs == BS_CONFIRM) {
    c = C_WARN; snprintf(a, sizeof a, "BOOT: GoTo"); snprintf(b, sizeof b, "A/B: cancel");
  } else if (bs == BS_READY && !trackEnabled()) {
    c = (int32_t)(millis() - flashUntil) < 0 ? C_WARN : C_LBL;
    if (altOk && sunNear) snprintf(a, sizeof a, "near the Sun");
    else if (altOk) snprintf(a, sizeof a, "below %d" DEG, (int)MIN_TRACK_ALT_DEG);
    else       snprintf(a, sizeof a, "alt unknown");
    snprintf(b, sizeof b, "no GoTo");
  } else if (bs == BS_READY) {
    snprintf(a, sizeof a, "BOOT: GoTo");
  }
  snprintf(key, sizeof key, "%s|%s|%04x", a, b, (unsigned)c);
  if (!strcmp(key, sideCache)) return;
  strncpy(sideCache, key, sizeof sideCache - 1);
  char p[16];
  snprintf(p, sizeof p, "%-12.12s", a); txt(SD_X, BT_Y + 3,  p, c, 1);
  snprintf(p, sizeof p, "%-12.12s", b); txt(SD_X, BT_Y + 16, p, c, 1);
}

static void drawLiveAlt() {                       // "Alt 45deg Az 120deg" under the left box
  char b[24], p[24];
  if (altOk) snprintf(b, sizeof b, "Alt %d" DEG " Az %d" DEG,
                      (int)lround(oAlt), (int)lround(oAz) % 360);
  else       snprintf(b, sizeof b, "Alt --");
  snprintf(p, sizeof p, "%-17.17s", b);
  char key[32];
  snprintf(key, sizeof key, "%s|%04x", p, (unsigned)altColor(altOk, oAlt));
  if (!strcmp(key, liveCache)) return;
  strncpy(liveCache, key, sizeof liveCache - 1);
  txt(2, 171, p, altColor(altOk, oAlt), 1);
}

static void drawHint() {
  const char *a = "A/B:prev/next", *b = "hold BOOT:back";
  uint16_t c = C_LBL;
  if (bs == BS_SLEW)         { a = "any key: STOP"; b = ""; c = C_WARN; }
  else if (bs == BS_CONFIRM) { a = "BOOT:GO"; b = "other:cancel"; }
  char key[40];
  snprintf(key, sizeof key, "%s|%s|%04x", a, b, (unsigned)c);
  if (!strcmp(key, hintCache)) return;
  strncpy(hintCache, key, sizeof hintCache - 1);
  char p[20];
  snprintf(p, sizeof p, "%-17.17s", a); txt(2, 216, p, c, 1);
  snprintf(p, sizeof p, "%-17.17s", b); txt(2, 228, p, c, 1);
}

static void objRefresh() { drawButton(); drawSide(); drawHint(); drawMs = millis(); }

static void drawInfoPane() {
  char b[48];
  tft.drawRect(RB_X, 0, RB_W, SCR_H, C_BOX);
  txt(TX_X, 4, "Object info", C_HEAD, 2);
  tft.drawFastHLine(RB_X + 1, 22, RB_W - 2, C_BOX);
  // summary: type  con  mag  size
  int n = snprintf(b, sizeof b, "%s  %s", obj->type, obj->con);
  if (obj->mag10 != INT16_MIN && n < (int)sizeof b)
    n += snprintf(b + n, sizeof b - n, "  mag %.1f", obj->mag10 / 10.0);
  if (obj->size10 > 0 && n < (int)sizeof b)
    snprintf(b + n, sizeof b - n, obj->size10 >= 100 ? "  %.0f'" : "  %.1f'", obj->size10 / 10.0);
  b[TX_COLS] = 0;
  txt(TX_X, 27, b, C_OK, 1);
  tft.drawFastHLine(RB_X + 4, 38, RB_W - 8, C_SEP);

  int nl = skyReadInfo(obj, info, 4), y = TX_Y;
  if (nl <= 0) { txt(TX_X, y, "(no info file on card)", C_LBL, 1); return; }
  y = wrapDraw(TX_X, y, TX_MAXY, info[0], TX_COLS, C_VAL) + 5;
  for (int i = 1; i < nl && y + 8 <= TX_MAXY; i++) {     // "Scope: ..." etc.
    char *v = info[i];
    char *colon = strchr(v, ':');
    int ll = (colon && colon - v < 12) ? (int)(colon - v) + 1 : 0;
    if (ll) {
      char lb[16];
      memcpy(lb, v, ll); lb[ll] = 0;
      txt(TX_X, y, lb, C_HEAD, 1);
      v += ll;
    }
    int ny = wrapDraw(TX_X, y, TX_MAXY, v, TX_COLS, C_VAL, ll ? ll + 1 : 0);
    y = (ny == y ? y + 10 : ny) + 3;
  }
}

static void objEnter(int i) {
  int n = skyCount(listCat);
  if (n <= 0) return;
  i = (i % n + n) % n;
  const SkyObj *o = skyGet(listCat, i);
  if (!o) return;
  listSel[listCat] = i; obj = o;
  scr = SC_OBJECT; flashUntil = 0;
  bool tracked = trackCat == listCat && trackIdx == i && mountTracking();
  bsSet(tracked ? BS_TRACK : BS_READY);
  btnCache[0] = sideCache[0] = liveCache[0] = hintCache[0] = 0;
  tft.fillScreen(BG);

  // left box: thumbnail, label, name, type. SKY_THUMB_PX < 96 (LittleFS build)
  // centers the smaller thumbnail in the same 96x96 placeholder box.
  tft.drawRect(0, 0, LB_W, LB_H, C_BOX);
  tft.fillRect(4, 4, 96, 96, C_SEP);
  if (!skyDrawImage(obj, 4 + (96 - SKY_THUMB_PX) / 2, 4 + (96 - SKY_THUMB_PX) / 2))
    txt(28, 48, "no image", C_LBL, 1, C_SEP);
  char b[24];
  if (strlen(obj->label) <= 8) txt(4, 104, obj->label, C_VAL, 2);
  else { snprintf(b, sizeof b, "%.16s", obj->label); txt(4, 108, b, C_VAL, 1); }
  wrapDraw(4, 122, 152, obj->name, 16, C_HEAD);
  snprintf(b, sizeof b, "%.16s", obj->type);
  txt(4, 154, b, C_LBL, 1);

  // below the box: J2000 catalogue position (live alt/az drawn by drawLiveAlt)
  double pra = obj->raH, pdec = obj->decD, pjd;  // planets: where it is now
  if (obj->cat == CAT_PLANET && !(skyNowJD(pjd) && skyObjNow(obj, pjd, pra, pdec))) pra = pdec = NAN;
  if (isnan(pra)) { txt(2, 183, "RA --", C_LBL, 1); txt(2, 195, "Dec --", C_LBL, 1); }
  else { fmtRA(pra, b, sizeof b, false); txt(2, 183, b, C_VAL, 1);
         fmtDec(pdec, b, sizeof b);      txt(2, 195, b, C_VAL, 1); }

  drawInfoPane();
  calcAlt();
  drawLiveAlt();
  objRefresh();
}

static void doGoto() {
  if (btConnected) { bsMsg("BT BUSY", C_WARN, false, RESULT_MS); return; }
  double jd, ra, dec;
  if (!skyNowJD(jd)) { bsMsg("NO TIME", C_BAD, true, RESULT_MS); return; }
  calcAlt(); drawLiveAlt();
  if (!trackEnabled()) { bsSet(BS_READY); flashUntil = millis() + FLASH_MS; return; }
  if (!skyObjNow(obj, jd, ra, dec)) { bsMsg("NO POS", C_BAD, true, RESULT_MS); return; }
  bsMsg("GOTO...", C_WARN, true, 10000);
  drawButton();                                   // feedback before the ~1 s block
  switch (mountGoto(ra, dec)) {
    case GOTO_OK:
      seenSlew = false; mountSetFastStatus(true); bsSet(BS_SLEW);
      trackCat = listCat; trackIdx = listSel[listCat];
      break;
    case GOTO_REJECTED: bsMsg("BELOW HRZN", C_BAD, true, RESULT_MS); break;
    case GOTO_NO_LINK:  bsMsg("NO MOUNT", C_BAD, true, RESULT_MS); break;
    case GOTO_BT_BUSY:  bsMsg("BT BUSY", C_WARN, false, RESULT_MS); break;
    default:            bsMsg("COMM ERR", C_BAD, true, RESULT_MS); break;
  }
}

static void objEvent(BtnEvent e) {
  if (bs == BS_SLEW) {                            // any button aborts the slew
    mountStop();
    mountSetFastStatus(false);
    bsMsg("STOPPED", C_BAD, true, RESULT_MS);
    return;
  }
  switch (e) {
    case EV_UP: case EV_DOWN:
      if (bs == BS_CONFIRM) bsSet(BS_READY);      // cancel
      else objEnter(listSel[listCat] + (e == EV_UP ? -1 : 1));   // prev/next (tracking continues)
      break;
    case EV_SEL:
      if (bs == BS_CONFIRM) doGoto();
      else if (bs == BS_TRACK) {}                 // already tracking this object
      else {
        bsSet(BS_READY);
        if (trackEnabled()) bsSet(BS_CONFIRM);
        else flashUntil = millis() + FLASH_MS;    // disabled: flash the note only
      }
      break;
    case EV_BACK:
      if (bs == BS_CONFIRM) bsSet(BS_READY);      // cancel the armed GoTo
      else listEnter(listCat);
      break;
    default: break;                               // hold-repeat: ignored here (thumb reload)
  }
}

static void slewTick() {
  if (M.statusCode == 2) seenSlew = true;
  uint32_t el = millis() - bsMs;
  if (el > SLEW_MAX_MS) {
    mountStop(); mountSetFastStatus(false);
    bsMsg("TIMEOUT", C_BAD, true, 5000);
  } else if ((seenSlew || el > 3000) && M.statusCode != 2) {
    if (M.statusCode == 0) mountSetTracking(true);
    mountSetFastStatus(false);
    bsSet(BS_TRACK);
  }
}

static void objTick(bool force) {
  uint32_t now = millis();
  if (bs == BS_SLEW && buttonsAnyDown()) {        // instant stop, then swallow that press
    mountStop();
    mountSetFastStatus(false);
    bsMsg("STOPPED", C_BAD, true, RESULT_MS);
    ignoreUntil = now + STOP_IGNORE_MS;
  }
  if (bs == BS_CONFIRM && now - bsMs > CONFIRM_MS) bsSet(BS_READY);
  if (bs == BS_MSG && now - bsMs > msgDur) bsSet(BS_READY);
  if (bs == BS_SLEW) slewTick();
  // stopped elsewhere (menu, hand controller): back to TRACK. Grace period
  // covers the poll that still shows "stopped" right after :ST1#.
  if (bs == BS_TRACK && now - bsMs > 5000 && !mountTracking()) { trackCat = -1; bsSet(BS_READY); }
  if (now - altMs >= ALT_OBJ_MS) { calcAlt(); drawLiveAlt(); }
  if (force || now - drawMs >= 100) objRefresh();
}

// ---- BLUETOOTH ----------------------------------------------------------------
// Rows: Protocol / up to 6 paired devices / Forget all / Back. The controller is
// always visible and pairs without a code (bt_bridge.h), so there is nothing to
// start here. Protocol (BOOT cycles iOptron -> LX200 -> NexStar) is what a
// Bluetooth client speaks: iOptron = transparent, else translated (xlate.h).
static const int BTS_Y = 30, BTS_H = 20, BT_MAXDEV = 6;
static int  btSel = 0, btDev = 0, btArmed = -1;   // btArmed: row awaiting its 2nd BOOT
static uint32_t btArmMs = 0, btSerialSeen = 0;
static bool btConnSeen = false;

static const int BT_PROTO = 0;                    // row index of "Protocol"
static int  btRowCount()   { return 1 + btDev + (btDev ? 1 : 0) + 1; }
static bool btIsDev(int i) { return i >= 1 && i <= btDev; }
static bool btIsAll(int i) { return btDev && i == btDev + 1; }

static void btRow(int i) {
  char b[40];
  uint16_t fg = C_VAL;
  bool sel = i == btSel;
  uint16_t bg = sel ? C_SEL : BG;
  int y = BTS_Y + i * BTS_H;
  if (i == BT_PROTO) {
    snprintf(b, sizeof b, "Protocol: %s", xlateModeName(xlateMode()));
    if (xlateMode() != XL_NATIVE) fg = C_HEAD;
  } else if (btIsDev(i)) {
    uint8_t mac[6]; char nm[24];
    if (btArmed == i) { snprintf(b, sizeof b, "Forget? BOOT again"); fg = C_WARN; }
    else if (btBondGet(i - 1, mac, nm, sizeof nm)) snprintf(b, sizeof b, "%.23s", nm);
    else b[0] = 0;
  } else if (btIsAll(i)) {
    if (btArmed == i) { snprintf(b, sizeof b, "Forget ALL? BOOT again"); fg = C_WARN; }
    else snprintf(b, sizeof b, "Forget all devices");
  } else snprintf(b, sizeof b, "Back");
  tft.fillRect(0, y, SCR_W, BTS_H - 2, bg);
  if (sel) tft.fillRect(0, y, 4, BTS_H - 2, C_HEAD);
  txt(14, y + 2, b, fg, 2, bg);
}

static void btStatusLine() {
  char b[56], p[56];
  uint16_t c = C_LBL;
  if (btConnected) { snprintf(b, sizeof b, "Connected - %s %s", xlateModeName(xlateMode()),
                              xlateMode() == XL_NATIVE ? "bridge active" : "translation"); c = C_WARN; }
  else snprintf(b, sizeof b, "Visible as %s - pair from any phone/PC", BT_NAME);
  snprintf(p, sizeof p, "%-52.52s", b);
  txt(4, 214, p, c, 1);
  btConnSeen = btConnected;
}

static void btDrawAll() {
  titleBar("Bluetooth");
  int n = btBondCount();
  btDev = n > BT_MAXDEV ? BT_MAXDEV : n;
  if (btSel >= btRowCount()) btSel = btRowCount() - 1;
  btArmed = -1;
  for (int i = 0; i < btRowCount(); i++) btRow(i);
  btStatusLine();
  footHint("A:up  B:down  BOOT:select  hold BOOT:back");
  btSerialSeen = btBondSerial();
}

static void btEnter() { scr = SC_BT; btSel = 0; btDrawAll(); }

static void btEvent(BtnEvent e) {
  int o = btSel, n = btRowCount();
  switch (e) {
    case EV_UP: case EV_UP_REP: case EV_DOWN: case EV_DOWN_REP: {
      int a = btArmed; btArmed = -1;
      btSel = (btSel + ((e == EV_UP || e == EV_UP_REP) ? -1 : 1) + n) % n;
      if (a >= 0 && a != o) btRow(a);
      btRow(o); btRow(btSel);
      break;
    }
    case EV_SEL:
      if (btSel == BT_PROTO) {
        protoCycle();
        btRow(BT_PROTO); btStatusLine();
      } else if (btIsDev(btSel) || btIsAll(btSel)) {
        if (btArmed != btSel) {                   // first press only arms
          int a = btArmed; btArmed = btSel; btArmMs = millis();
          if (a >= 0) btRow(a);
          btRow(btSel);
        } else {                                  // bond serial bump -> full redraw
          btArmed = -1;
          if (btIsDev(btSel)) btBondForget(btSel - 1); else btBondForgetAll();
          setMsg("Bluetooth device forgotten", C_OK, 3000);
        }
      } else menuEnter();
      break;
    case EV_BACK:
      if (btArmed >= 0) { int a = btArmed; btArmed = -1; btRow(a); }
      else menuEnter();
      break;
    default: break;
  }
}

static void btScreenTick() {
  if (btBondSerial() != btSerialSeen) { btDrawAll(); return; }
  if (btArmed >= 0 && millis() - btArmMs > CONFIRM_MS) { int a = btArmed; btArmed = -1; btRow(a); }
  if (btConnected != btConnSeen) btStatusLine();
}

// ---- DEBUG LOG ------------------------------------------------------------------
// The LOG() output (debuglog.h) as it happens: 19 lines, newest at the bottom,
// coloured by source. A = older, B = newer (scrolled = paused), BOOT = live,
// hold BOOT = back.
static const int DBG_Y = 30, DBG_ROWS = 19;
static Screen   dbgBack = SC_STATUS;
static int      dbgOff = 0;                       // lines scrolled back from the newest
static uint32_t dbgSeen = 0, dbgMs = 0;
static char     dbgHdr[40];

static uint16_t dbgColor(const char *s) {
  if (strstr(s, "FAIL") || strstr(s, "REJECT") || strstr(s, "rejected") || strstr(s, "failed"))
    return C_BAD;
  if (s[0] == '>' || !strncmp(s, "[cons]", 6)) return C_WARN;
  if (!strncmp(s, "[bt]", 4))  return C_HEAD;
  if (!strncmp(s, "[msg]", 5)) return C_OK;
  if (!strncmp(s, "[xl]", 4))  return C_VAL;
  return C_LBL;
}

static void dbgHeader() {                         // right of the title: BT / mount state
  char b[40];
  if (dbgOff) snprintf(b, sizeof b, "PAUSED -%d", dbgOff);
  else snprintf(b, sizeof b, "BT %s %d  M %s", btConnected ? "conn" : "idle", btBondCount(),
                linkUp ? "ok" : "--");
  char p[40];
  snprintf(p, sizeof p, "%21.21s", b);
  if (!strcmp(p, dbgHdr)) return;
  strncpy(dbgHdr, p, sizeof dbgHdr - 1);
  txt(SCR_W - 4 - 21 * 6, 10, p, dbgOff ? C_WARN : C_LBL, 1);
}

static void dbgDrawLines() {
  char ln[DBG_COLS + 1], p[DBG_COLS + 1];
  for (int r = 0; r < DBG_ROWS; r++) {
    int back = dbgOff + DBG_ROWS - 1 - r;
    if (!dbgLine(back, ln, sizeof ln)) ln[0] = 0;
    snprintf(p, sizeof p, "%-*.*s", DBG_COLS, DBG_COLS, ln);
    txt(0, DBG_Y + r * 10, p, dbgColor(ln), 1);
  }
  dbgSeen = dbgSeq(); dbgMs = millis();
}

static void dbgDrawAll() {
  titleBar("Debug log");
  dbgHdr[0] = 0;
  dbgHeader();
  dbgDrawLines();
  footHint("A:older  B:newer  BOOT:live  hold BOOT:back");
}

static void dbgEnter(Screen back) { scr = SC_DEBUG; dbgBack = back; dbgOff = 0; dbgDrawAll(); }

static void dbgEvent(BtnEvent e) {
  int o = dbgOff;
  switch (e) {
    case EV_UP: case EV_UP_REP:     dbgOff += e == EV_UP ? 1 : DBG_ROWS; break;
    case EV_DOWN: case EV_DOWN_REP: dbgOff -= e == EV_DOWN ? 1 : DBG_ROWS; break;
    case EV_SEL:  dbgOff = 0; break;
    case EV_BACK:
      if (dbgBack == SC_MENU) menuEnter(); else statusEnter();
      return;
    default: break;
  }
  int maxOff = (int)min<uint32_t>(dbgSeq(), DBG_RING) - DBG_ROWS;
  dbgOff = constrain(dbgOff, 0, max(0, maxOff));
  if (dbgOff != o) { dbgHeader(); dbgDrawLines(); }
}

static void dbgTick(bool force) {
  dbgHeader();
  if (force || (!dbgOff && dbgSeq() != dbgSeen && millis() - dbgMs >= 150)) dbgDrawLines();
}

void uiShowDebug(bool on) {
  if (on && scr != SC_DEBUG) {
    if (scr == SC_OBJECT && bs == BS_SLEW) return; // keep the STOP button up
    dbgEnter(scr == SC_STATUS ? SC_STATUS : SC_MENU);
  } else if (!on && scr == SC_DEBUG) {
    if (dbgBack == SC_MENU) menuEnter(); else statusEnter();
  }
}

bool uiDebugShown() { return scr == SC_DEBUG; }

// ---- entry points ---------------------------------------------------------------
void uiBegin() {
  tft.setTextWrap(false);
  scr = SC_STATUS;
}

void uiTick() {
  BtnEvent e = buttonsGet();                      // one event per loop pass
  if (e != EV_NONE && (int32_t)(ignoreUntil - millis()) > 0)
    e = EV_NONE;                                  // tail of the press that stopped a slew

  if (zeroSlew) {
    if (buttonsAnyDown()) {                       // instant stop, then swallow that press
      zeroStop("Zero slew STOPPED", C_BAD);
      ignoreUntil = millis() + STOP_IGNORE_MS;
      e = EV_NONE;
    } else zeroTick();
  }
  if (zeroArmed && millis() - zeroArmMs > CONFIRM_MS) zeroDisarm();
  if (resetArmed && millis() - resetArmMs > CONFIRM_MS) resetDisarm();

  if ((scr == SC_LIST || scr == SC_OBJECT) && !skyReady() && !(scr == SC_OBJECT && bs == BS_SLEW)) {
    setMsg(SKY_LOST_MSG, C_WARN, 4000);
    statusEnter();
    return;
  }

  switch (scr) {
    case SC_STATUS:
      if (e != EV_NONE) statusEvent(e);
      if (scr == SC_STATUS) displayTick();
      break;
    case SC_MENU:
      if (e != EV_NONE) menuEvent(e);
      break;
    case SC_LIST:
      if (e != EV_NONE) listEvent(e);
      else if (millis() - listAltMs >= ALT_LIST_MS) listRefreshAlt();
      break;
    case SC_OBJECT:
      if (e != EV_NONE) objEvent(e);
      if (scr == SC_OBJECT) objTick(e != EV_NONE);
      break;
    case SC_BT:
      if (e != EV_NONE) btEvent(e);
      if (scr == SC_BT) btScreenTick();
      break;
    case SC_DEBUG:
      if (e != EV_NONE) dbgEvent(e);
      if (scr == SC_DEBUG) dbgTick(false);
      break;
  }
}
