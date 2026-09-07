#include "display.h"
#include "config.h"
#include "mount.h"
#include "bt_bridge.h"
#include "gps.h"
#include "gps_sync.h"
#include "message.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <string.h>

static Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
static const int SCR_W = 320;

struct Row { int16_t y; uint8_t sz; const char *label; char cache[34]; };
static Row rows[] = {
  {  30, 1, "Model", "" }, {  42, 1, "FW",    "" },
  {  56, 2, "Time",  "" }, {  78, 2, "Date",  "" },
  { 100, 1, "Zone",  "" }, { 112, 2, "Site",  "" },
  { 136, 2, "Stat",  "" }, { 158, 1, "Trk",   "" }, { 170, 1, "Pos", "" },
};
enum { R_MODEL, R_FW, R_TIME, R_DATE, R_ZONE, R_SITE, R_STAT, R_TRK, R_POS };
static char hdrCache[28] = "", gpsCache[40] = "", ftrCache[40] = "";

static void txt(int x, int y, const char *s, uint16_t fg, uint8_t sz) {
  tft.setTextSize(sz); tft.setTextColor(fg, BG); tft.setCursor(x, y); tft.print(s);
}

static void staticLayout() {
  tft.fillScreen(BG);
  tft.drawFastHLine(0, 26, SCR_W, C_HEAD);
  tft.drawFastHLine(0, 52, SCR_W, C_SEP);
  tft.drawFastHLine(0, 132, SCR_W, C_SEP);
  tft.drawFastHLine(0, 186, SCR_W, C_SEP);
  for (auto &r : rows) txt(4, r.y + (r.sz == 2 ? 2 : 0), r.label, C_LBL, 1);
}

static void row(int i, const char *val, uint16_t color) {
  Row &r = rows[i];
  int w = r.sz == 2 ? 13 : 8;                 // pad width in chars
  char pad[40];
  snprintf(pad, sizeof pad, "%-*.*s", 26 - w, 26 - w, val);
  if (!strcmp(r.cache, pad)) return;
  strncpy(r.cache, pad, sizeof r.cache - 1);
  txt(w * 6 + 6, r.y, pad, color, r.sz);
}

static void header() {
  const char *st = btConnected ? "BRIDGE" : (linkUp ? "MOUNT OK" : "NO MOUNT");
  char h[28];
  snprintf(h, sizeof h, "%s|%s", BT_NAME, st);
  if (strcmp(h, hdrCache)) {
    strncpy(hdrCache, h, sizeof hdrCache - 1);
    tft.fillRect(0, 2, SCR_W, 22, BG);
    txt(4, 4, BT_NAME, C_HEAD, 2);
    txt(4 + 12 * 12, 8, st, btConnected ? C_WARN : (linkUp ? C_OK : C_BAD), 1);
  }
}

static void gpsBlock() {
  char s[40], line[40];
  gpsStatusStr(s, sizeof s);
  bool lk = gpsLocked();
  snprintf(line, sizeof line, "GPS: %-18s", s);
  if (strcmp(line, gpsCache)) {
    strncpy(gpsCache, line, sizeof gpsCache - 1);
    tft.fillRect(0, 192, SCR_W, 18, BG);
    txt(4, 192, "GPS:", C_LBL, 2);
    txt(4 + 5 * 12, 192, s, lk ? C_OK : C_WARN, 2);
  }
  static char fx[40] = "";
  char d[40];
  snprintf(d, sizeof d, "%-38s", syncMsg);
  if (strcmp(d, fx)) {
    strncpy(fx, d, sizeof fx - 1);
    tft.fillRect(0, 212, SCR_W, 10, BG);
    txt(4, 212, syncMsg, syncMsgColor, 1);
  }
}

static void footer() {
  char f[40];
  if (millis() < gMsgUntil) {
    snprintf(f, sizeof f, "%-38s", gMsg);
    if (strcmp(f, ftrCache)) {
      strncpy(ftrCache, f, sizeof ftrCache - 1);
      tft.fillRect(0, 228, SCR_W, 12, BG);
      txt(4, 229, gMsg, gMsgColor, 1);
    }
    return;
  }
  if (!M.everOk)        snprintf(f, sizeof f, "waiting for mount...        ");
  else if (btConnected) snprintf(f, sizeof f, "bridging - values held      ");
  else snprintf(f, sizeof f, "mount src:%s  upd %lus       ",
                M.gpsSrc[0] ? M.gpsSrc : "?", (millis() - M.lastOkMs) / 1000);
  if (strcmp(f, ftrCache)) {
    strncpy(ftrCache, f, sizeof ftrCache - 1);
    tft.fillRect(0, 228, SCR_W, 12, BG);
    txt(4, 229, f, C_LBL, 1);
  }
}

static void render() {
  char b[48];
  header();

  snprintf(b, sizeof b, "%s (%s)", M.modelName[0] ? M.modelName : "-", M.model);
  row(R_MODEL, b, C_VAL);
  snprintf(b, sizeof b, "%s %s/%s", M.proto[0] ? M.proto : "-",
           M.fwMain[0] ? M.fwMain : "-", M.fwMotor[0] ? M.fwMotor : "-");
  row(R_FW, b, C_VAL);

  row(R_TIME, M.timeStr[0] ? M.timeStr : "--:--:--", C_VAL);
  row(R_DATE, M.dateStr[0] ? M.dateStr : "----------", C_VAL);
  snprintf(b, sizeof b, "UTC%s  DST %s", M.utcOffset[0] ? M.utcOffset : "?",
           M.dst[0] ? M.dst : "?");
  row(R_ZONE, b, C_VAL);
  snprintf(b, sizeof b, "%s %s", M.lonStr[0] ? M.lonStr : "lon?",
           M.latStr[0] ? M.latStr : "lat?");
  row(R_SITE, b, C_VAL);

  bool moving = !strcmp(M.statusStr, "Slewing") || !strcmp(M.statusStr, "Guiding");
  snprintf(b, sizeof b, "%s  %s", M.statusStr[0] ? M.statusStr : "-",
           M.slewRate[0] ? M.slewRate : "?");
  row(R_STAT, b, moving ? C_WARN : C_VAL);
  snprintf(b, sizeof b, "%s g%s", M.trackStr[0] ? M.trackStr : "-",
           M.guideStr[0] ? M.guideStr : "?");
  row(R_TRK, b, C_VAL);
  snprintf(b, sizeof b, "RA %s Dec %s", M.raStr[0] ? M.raStr : "--h--m", M.decStr);
  row(R_POS, b, C_VAL);

  gpsBlock();
  footer();
}

void displayBegin() {
  tft.init(240, 320);
  tft.setSPISpeed(TFT_HZ);
  tft.invertDisplay(TFT_INVERT);
  tft.setRotation(1);              // 320x240 landscape
  LOG("[tft] %d x %d\n", tft.width(), tft.height());
  staticLayout();
  header();
  footer();
}

void displayTick() {
  static uint32_t t = 0;
  if (millis() - t < 250) return;
  t = millis();
  render();
}
