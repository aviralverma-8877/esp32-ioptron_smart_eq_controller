// ESP32 RJ9 Adapter - BT SPP <-> RS-232 bridge + status TFT + GPS time/site sync
// ---------------------------------------------------------------------------
// * Classic Bluetooth "serial port" (SPP) <-> MAX3232 / RJ9  transparent bridge
//   for driving an iOptron mount (verified: SmartEQ Pro, MountInfo 0011, 9600 8N1).
// * ILI-class TFT (ST7789, 320x240) shows the mount's own config: model, firmware,
//   time, UTC offset / DST, site lon/lat, RA/Dec, tracking & guide rates, status.
// * On-board NEO-6M GPS (NMEA in on GPIO32). Screen shows lock state + fix.
// * Button BTN1 (GPIO39): when GPS is locked, push GPS date/time + site
//   longitude/latitude into the mount. If not locked, prompts "GPS NOT LOCKED".
//   Only runs while no BT client is connected (bridge stays transparent otherwise).
//
//   BT SPP client <-RFCOMM-> ESP32 UART2 (GPIO17/16) <-> MAX3232 <-RS232-> RJ9 -> mount
//   TFT  (ST7789, HW SPI/VSPI): SCLK18 MOSI23 MISO19  CS25 DC26 RST27   (backlight->3v3)
//   GPS  (NEO-6M): TX -> GPIO32  (ESP32 UART1 RX, 9600 8N1, receive only)
//   BTN1 GPIO39 (active-low, ext. pull-up) ;  Activity LED GPIO2 (active-high)
//
//   *** rev 1.0 hw note *** RJ1 pin 3/4 (TXD/RXD) are swapped vs a straight
//   iOptron RJ9 cable - use a cable that swaps pins 3<->4 (pin 1 GND straight).
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <TinyGPSPlus.h>
#include "BluetoothSerial.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error "Classic Bluetooth is not enabled (use the default esp32dev Bluedroid build)."
#endif

// ---- configuration ------------------------------------------------------
#ifndef BRIDGE_BAUD
#define BRIDGE_BAUD 9600
#endif
#ifndef RS232_RX_PIN
#define RS232_RX_PIN 16
#endif
#ifndef RS232_TX_PIN
#define RS232_TX_PIN 17
#endif
#ifndef ACT_LED_PIN
#define ACT_LED_PIN 2
#endif
#ifndef GPS_RX_PIN
#define GPS_RX_PIN 32              // NEO-6M TX -> here
#endif
#ifndef GPS_BAUD
#define GPS_BAUD 9600
#endif
#ifndef BTN_SYNC_PIN
#define BTN_SYNC_PIN 39           // BTN1, active-low
#endif
#ifndef TFT_CS
#define TFT_CS 25
#endif
#ifndef TFT_DC
#define TFT_DC 26
#endif
#ifndef TFT_RST
#define TFT_RST 27
#endif
#ifndef TFT_HZ
#define TFT_HZ 24000000
#endif
#ifndef TFT_INVERT
#define TFT_INVERT 0
#endif
#ifndef BT_NAME
#define BT_NAME "SmartEQ-RJ9"
#endif
#ifndef DEBUG_BAUD
#define DEBUG_BAUD 115200
#endif
#ifndef POLL_PERIOD_MS
#define POLL_PERIOD_MS 400
#endif
#ifndef SD_CS_PIN
#define SD_CS_PIN 33
#endif
#ifndef TOUCH_CS_PIN
#define TOUCH_CS_PIN 13
#endif

#if defined(DEBUG_LOG) && DEBUG_LOG
#define LOG(...)  do { Serial.printf(__VA_ARGS__); } while (0)
#else
#define LOG(...)  do {} while (0)
#endif

// ---- colours (RGB565) ------------------------------------------------
#define BG      0x0000
#define C_HEAD  0x07FF
#define C_LBL   0x8410
#define C_VAL   0xFFFF
#define C_OK    0x07E0
#define C_WARN  0xFFE0
#define C_BAD   0xF800
#define C_SEP   0x2124

// ---- globals ----------------------------------------------------------
static BluetoothSerial   Bt;
static HardwareSerial     Rs232(2);
static HardwareSerial     GpsSer(1);
static TinyGPSPlus        gps;
static Adafruit_ST7789    tft(TFT_CS, TFT_DC, TFT_RST);

static const size_t CHUNK          = 256;
static const size_t UART_RX_BUFFER = 2048;

static volatile bool btConnected   = false;
static uint32_t lastMountRxMs      = 0;   // last time the mount sent us anything
static bool     linkUp             = false;

// mount link = something heard from the mount recently. Poll keeps it fresh
// when idle; client traffic keeps it fresh while bridging.
static bool mountLinkUp() {
  uint32_t to = btConnected ? 8000 : 2500;
  return lastMountRxMs && (millis() - lastMountRxMs < to);
}

// transient on-screen message (footer)
static char     gMsg[30]   = "";
static uint16_t gMsgColor   = 0xFFFF;
static uint32_t gMsgUntil    = 0;
static void setMsg(const char *m, uint16_t c, uint32_t ms) {
  strncpy(gMsg, m, sizeof gMsg - 1); gMsg[sizeof gMsg - 1] = 0;
  gMsgColor = c; gMsgUntil = millis() + ms;
  LOG("[msg] %s\n", m);
}

// ---- decoded mount state --------------------------------------------
struct MountState {
  char model[6]      = "----";
  char modelName[16] = "";
  char proto[8]      = "";
  char fwMain[10]    = "";
  char fwMotor[10]   = "";
  char utcOffset[8]  = "";
  char dst[4]        = "";
  char dateStr[12]   = "";
  char timeStr[10]   = "";
  char lonStr[14]    = "";
  char latStr[14]    = "";
  char statusStr[12] = "";
  char trackStr[10]  = "";
  char slewRate[6]   = "";
  char guideStr[12]  = "";
  char raStr[12]     = "";
  char decStr[12]    = "";
  char hemi[6]       = "";
  char gpsSrc[8]     = "";        // mount's own GPS/time source flag
  long offsetMin     = 0;
  int  dstFlag       = 0;
  uint32_t lastOkMs  = 0;
  bool     everOk    = false;
};
static MountState M;

// ---- small helpers -------------------------------------------------
// one direction of the transparent bridge; note when the mount talks to us
static void pump(Stream &in, Print &out, bool fromMount) {
  uint8_t buf[CHUNK];
  int avail;
  while ((avail = in.available()) > 0) {
    size_t want = avail > (int)CHUNK ? CHUNK : (size_t)avail;
    size_t got  = in.readBytes(buf, want);
    if (got) {
      out.write(buf, got);
      if (fromMount) lastMountRxMs = millis();
    }
  }
}
static void drain(Stream &s) { while (s.available()) s.read(); }

static size_t askMount(const char *cmd, char *out, size_t outSz,
                       uint16_t timeoutMs, uint8_t fixedLen = 0) {
  drain(Rs232);
  Rs232.write((const uint8_t *)cmd, strlen(cmd));
  size_t n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs && n < outSz - 1) {
    if (Rs232.available()) {
      char c = (char)Rs232.read();
      out[n++] = c;
      if (fixedLen == 0 && c == '#') break;
      if (fixedLen && n >= fixedLen) break;
      t0 = millis();
    }
  }
  out[n] = 0;
  if (n > 0) lastMountRxMs = millis();
  return n;
}

static bool allDigits(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
  return n > 0;
}
static long sub2l(const char *s, int off, int len) {
  char b[12]; if (len > 11) len = 11;
  memcpy(b, s + off, len); b[len] = 0; return atol(b);
}

// civil <-> days-since-epoch (Howard Hinnant), for UTC->local calendar math
static long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  int  yoe = y - (int)(era * 400);
  int  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
static void civilFromDays(long z, int &y, int &m, int &d) {
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  int  doe = (int)(z - era * 146097);
  int  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = yoe + (int)(era * 400);
  int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int mp  = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y += (m <= 2);
}

// ---- iOptron reply decoders -------------------------------------
static const char *modelName(const char *code) {
  struct { const char *c, *n; } t[] = {
    {"0010","SmartEQ"},     {"0011","SmartEQ Pro"}, {"0025","CEM25"},
    {"0026","CEM26"},       {"0027","CEM26-EC"},    {"0028","GEM28"},
    {"0030","iEQ30 Pro"},   {"0040","CEM40"},       {"0041","CEM40-EC"},
    {"0043","GEM45"},       {"0044","GEM45-EC"},    {"0045","iEQ45 Pro"},
    {"0046","iEQ45 Pro AA"},{"0060","CEM60"},       {"0061","CEM60-EC"},
    {"0070","CEM70"},       {"0071","CEM70-EC"},    {"5010","Cube II EQ"},
    {"5035","AZ Mount Pro"},
  };
  for (auto &e : t) if (!strcmp(e.c, code)) return e.n;
  return "mount";
}

static void decGLT(const char *r) {                // sMMMd YYMMDD HHMMSS #
  if (strlen(r) < 17 || (r[0] != '+' && r[0] != '-')) return;
  M.offsetMin = sub2l(r, 1, 3) * (r[0] == '-' ? -1 : 1);
  M.dstFlag   = r[4] - '0';
  long ymd = sub2l(r, 5, 6), hms = sub2l(r, 11, 6);
  int Y = 2000 + ymd / 10000, Mo = (ymd / 100) % 100, D = ymd % 100;
  int h = hms / 10000, mi = (hms / 100) % 100, s = hms % 100;
  snprintf(M.utcOffset, sizeof M.utcOffset, "%c%02ld:%02ld",
           M.offsetMin < 0 ? '-' : '+', labs(M.offsetMin) / 60, labs(M.offsetMin) % 60);
  snprintf(M.dst, sizeof M.dst, "%s", M.dstFlag ? "on" : "off");
  snprintf(M.dateStr, sizeof M.dateStr, "%04d-%02d-%02d", Y, Mo, D);
  snprintf(M.timeStr, sizeof M.timeStr, "%02d:%02d:%02d", h, mi, s);
}
static void decGG(const char *r) {                 // longitude, 0.01"  (:Gg#)
  if (strlen(r) < 5 || (r[0] != '+' && r[0] != '-')) return;
  double d = atol(r + 1) / 360000.0 * (r[0] == '-' ? -1 : 1);
  snprintf(M.lonStr, sizeof M.lonStr, "%.4f%c", fabs(d), d < 0 ? 'W' : 'E');
}
static void decGT(const char *r) {                 // latitude, 0.01"  (:Gt#)
  if (strlen(r) < 5 || (r[0] != '+' && r[0] != '-')) return;
  double d = atol(r + 1) / 360000.0 * (r[0] == '-' ? -1 : 1);
  snprintf(M.latStr, sizeof M.latStr, "%.4f%c%s", fabs(d), d < 0 ? 'S' : 'N',
           fabs(d) > 90.0 ? "?" : "");
}
static void decGAS(const char *r) {                // G S T M X H
  if (strlen(r) < 6 || !allDigits(r, 6)) return;
  int st = r[1] - '0', tr = r[2] - '0', sp = r[3] - '0', hm = r[5] - '0';
  const char *S[] = {"Stopped","Tracking","Slewing","Guiding","Mrdn flip",
                     "Track+PEC","Parked","At home"};
  const char *T[] = {"Sidereal","Lunar","Solar","King","Custom"};
  const char *R[] = {"-","1x","2x","8x","16x","64x","128x","256x","512x","MAX"};
  const char *X[] = {"RS232","hand ctl","GPS"};
  snprintf(M.statusStr, sizeof M.statusStr, "%s", st >= 0 && st < 8 ? S[st] : "?");
  snprintf(M.trackStr,  sizeof M.trackStr,  "%s", tr >= 0 && tr < 5 ? T[tr] : "?");
  snprintf(M.slewRate,  sizeof M.slewRate,  "%s", sp >= 0 && sp < 10 ? R[sp] : "?");
  snprintf(M.hemi,      sizeof M.hemi,      "%s", hm ? "N" : "S");
  int xs = r[4] - '0';
  snprintf(M.gpsSrc,    sizeof M.gpsSrc,    "%s", xs >= 0 && xs < 3 ? X[xs] : "?");
}
static void decGEC(const char *r) {                // sDDDDDDDD RRRRRRRR # (0.01")
  if (strlen(r) < 17 || (r[0] != '+' && r[0] != '-')) return;
  double dec = sub2l(r, 1, 8) / 360000.0 * (r[0] == '-' ? -1 : 1);
  double raH = sub2l(r, 9, 8) / 360000.0 / 15.0;
  int rh = (int)raH, rm = (int)((raH - rh) * 60);
  int dd = (int)fabs(dec); int dm = (int)((fabs(dec) - dd) * 60);
  snprintf(M.raStr,  sizeof M.raStr,  "%02dh%02dm", rh, rm);
  snprintf(M.decStr, sizeof M.decStr, "%c%02d %02d'", dec < 0 ? '-' : '+', dd, dm);
}
static void decAG(const char *r) {                 // gggg -> x0.01
  if (strlen(r) < 4 || !allDigits(r, 4)) return;
  snprintf(M.guideStr, sizeof M.guideStr, "%.2f/%.2f",
           sub2l(r, 0, 2) / 100.0, sub2l(r, 2, 2) / 100.0);
}

// ---- self-poll state machine (only while no BT client) -----------
enum PollStep { P_MODEL, P_PROTO, P_FW1, P_FW2, P_GLT, P_GG, P_GT, P_GAS, P_GEC, P_AG, P_COUNT };
static uint8_t  pollStep    = P_MODEL;
static uint32_t pollLastMs   = 0;
static uint8_t  slowCounter  = 0;

static void pollOnce() {
  char r[40];
  switch (pollStep) {
    case P_MODEL:
      if (askMount(":MountInfo#", r, sizeof r, 500, 4) == 4) {
        strncpy(M.model, r, 5);
        snprintf(M.modelName, sizeof M.modelName, "%s", modelName(r));
      }
      break;
    case P_PROTO:
      if (askMount(":V#", r, sizeof r, 400)) {
        char *h = strchr(r, '#'); if (h) *h = 0;
        strncpy(M.proto, r, sizeof M.proto - 1);
      }
      break;
    case P_FW1:
      if (askMount(":FW1#", r, sizeof r, 500) >= 13)
        snprintf(M.fwMain, sizeof M.fwMain, "%c%c-%c%c-%c%c", r[0],r[1],r[2],r[3],r[4],r[5]);
      break;
    case P_FW2:
      if (askMount(":FW2#", r, sizeof r, 500) >= 13)
        snprintf(M.fwMotor, sizeof M.fwMotor, "%c%c-%c%c-%c%c", r[0],r[1],r[2],r[3],r[4],r[5]);
      break;
    case P_GLT: if (askMount(":GLT#", r, sizeof r, 500)) decGLT(r); break;
    case P_GG:  if (askMount(":Gg#",  r, sizeof r, 400)) decGG(r);  break;
    case P_GT:  if (askMount(":Gt#",  r, sizeof r, 400)) decGT(r);  break;
    case P_GAS: if (askMount(":GAS#", r, sizeof r, 500)) decGAS(r); break;
    case P_GEC: if (askMount(":GEC#", r, sizeof r, 500)) decGEC(r); break;
    case P_AG:  if (askMount(":AG#",  r, sizeof r, 400)) decAG(r);  break;
  }
  M.lastOkMs = millis();
  M.everOk = true;
  do {
    pollStep = (pollStep + 1) % P_COUNT;
    if (pollStep == P_MODEL) slowCounter++;
  } while ((pollStep == P_MODEL || pollStep == P_PROTO ||
            pollStep == P_FW1   || pollStep == P_FW2) && (slowCounter & 0x07));
}

// ---- GPS -------------------------------------------------------
static void feedGps() {
  while (GpsSer.available()) gps.encode(GpsSer.read());
}
static bool gpsLocked() {
  return gps.location.isValid() && gps.location.age() < 5000 &&
         gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024 &&
         gps.satellites.isValid() && gps.satellites.value() >= 4;
}
static void gpsStatusStr(char *out, size_t n) {
  if (millis() > 8000 && gps.charsProcessed() < 20) { snprintf(out, n, "no module"); return; }
  int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  if (gpsLocked())            snprintf(out, n, "LOCKED  %d sat", sats);
  else if (gps.charsProcessed()) snprintf(out, n, "acquiring  %d sat", sats);
  else                        snprintf(out, n, "no data");
}

// ---- automatic GPS -> mount sync ------------------------------------
// When the GPS holds a valid fix for GPS_LOCK_SETTLE_MS the firmware pushes
// date/time + site into the mount by itself (no button). It re-syncs every
// GPS_RESYNC_MS to trim clock drift, and re-arms whenever the fix is lost.
#define GPS_LOCK_SETTLE_MS   6000UL
#define GPS_RESYNC_MS        (3UL * 3600 * 1000)   // 3 hours

static bool     syncArmed    = true;    // re-armed on fix loss / periodic timer
static uint32_t lockSinceMs  = 0;       // millis when the present fix began
static uint32_t lastSyncMs   = 0;       // millis of last OK sync (0 = never)
static char     syncMsg[30]  = "waiting for GPS fix";
static uint16_t syncMsgColor = C_LBL;

// push GPS date/time + site into the mount (only when idle + locked)
static void doGpsSync(bool manual) {
  if (btConnected) {
    if (manual) setMsg("BT client active - cannot sync", 0xFFE0, 4000);
    snprintf(syncMsg, sizeof syncMsg, "sync held (BT active)"); syncMsgColor = C_WARN;
    return;
  }
  if (!gpsLocked()) {
    if (manual) setMsg("GPS NOT LOCKED - cannot sync", 0xF800, 4000);
    return;
  }

  setMsg("Syncing mount from GPS...", 0xFFE0, 9000);
  char r[40];

  // keep the mount's configured UTC offset + DST, refresh from :GLT#
  if (askMount(":GLT#", r, sizeof r, 600)) decGLT(r);

  long ep  = daysFromCivil(gps.date.year(), gps.date.month(), gps.date.day()) * 86400L
           + gps.time.hour() * 3600L + gps.time.minute() * 60L + gps.time.second();
  long loc = ep + M.offsetMin * 60L + (M.dstFlag ? 3600L : 0L);
  int Y, Mo, D; civilFromDays(loc / 86400, Y, Mo, D);
  long tod = loc % 86400; if (tod < 0) tod += 86400;
  int hh = tod / 3600, mi = (tod % 3600) / 60, ss = tod % 60;

  long lon001 = llround(gps.location.lng() * 360000.0);   // 0.01 arc-seconds
  long lat001 = llround(gps.location.lat() * 360000.0);

  int ok = 0;
  char c[24];
  snprintf(c, sizeof c, ":SC%02d%02d%02d#", Y % 100, Mo, D);
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;
  snprintf(c, sizeof c, ":SL%02d%02d%02d#", hh, mi, ss);
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;
  snprintf(c, sizeof c, ":Sg%c%08ld#", lon001 < 0 ? '-' : '+', labs(lon001));
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;
  snprintf(c, sizeof c, ":St%c%08ld#", lat001 < 0 ? '-' : '+', labs(lat001));
  if (askMount(c, r, sizeof r, 700) && r[0] == '1') ok++;

  LOG("[sync] %04d-%02d-%02d %02d:%02d:%02d local  lat %.5f lon %.5f  ok=%d/4\n",
      Y, Mo, D, hh, mi, ss, gps.location.lat(), gps.location.lng(), ok);

  if (ok == 4) {
    setMsg("GPS sync OK", 0x07E0, 6000);
    lastSyncMs = millis();
    snprintf(syncMsg, sizeof syncMsg, "synced %02d:%02d:%02dZ",
             gps.time.hour(), gps.time.minute(), gps.time.second());
    syncMsgColor = C_OK;
  } else {
    snprintf(c, sizeof c, "GPS sync incomplete %d/4", ok); setMsg(c, 0xFFE0, 8000);
    snprintf(syncMsg, sizeof syncMsg, "sync failed %d/4 - retrying", ok);
    syncMsgColor = C_BAD;
  }

  pollStep = P_GLT;            // refresh the displayed values next cycles
}

// call every loop: fire the sync automatically once the fix is stable
static void autoSyncTick() {
  bool locked = gpsLocked();
  uint32_t now = millis();

  if (!locked) {
    syncArmed = true;
    lockSinceMs = 0;
    if (!lastSyncMs) { strcpy(syncMsg, "waiting for GPS fix"); syncMsgColor = C_LBL; }
    else { snprintf(syncMsg, sizeof syncMsg, "fix lost - was synced"); syncMsgColor = C_WARN; }
    return;
  }
  if (!lockSinceMs) lockSinceMs = now;
  if (lastSyncMs && now - lastSyncMs > GPS_RESYNC_MS) syncArmed = true;   // periodic re-sync

  if (btConnected) { snprintf(syncMsg, sizeof syncMsg, "sync held (BT active)");
                     syncMsgColor = C_WARN; return; }
  if (!syncArmed) return;

  uint32_t held = now - lockSinceMs;
  if (held < GPS_LOCK_SETTLE_MS) {
    snprintf(syncMsg, sizeof syncMsg, "fix ok - syncing in %lus",
             (unsigned long)((GPS_LOCK_SETTLE_MS - held) / 1000 + 1));
    syncMsgColor = C_WARN;
    return;
  }
  syncArmed = false;
  doGpsSync(false);
}

// BTN1 (active-low, external pull-up). An ISR latches the press so it is never
// missed while loop() is blocked in a serial read; loop() consumes the latch.
static volatile bool     btnLatch = false;
static volatile uint32_t btnIsrMs = 0;
static void IRAM_ATTR btnIsr() {
  uint32_t m = millis();
  if (m - btnIsrMs > 250) { btnLatch = true; btnIsrMs = m; }   // debounce
}
static void checkButton() {                        // optional manual re-sync
  if (!btnLatch) return;
  btnLatch = false;
  if (digitalRead(BTN_SYNC_PIN) != LOW) return;   // reject a glitch
  LOG("[btn] PRESS\n");
  setMsg("button - re-syncing from GPS...", C_WARN, 2500);
  syncArmed = false;
  doGpsSync(true);
}

// ---- TFT rendering (redraw only changed fields) --------------
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
  char h[28];
  const char *st = btConnected ? "BRIDGE" : (linkUp ? "MOUNT OK" : "NO MOUNT");
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
  // fix detail line
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

static void tftUpdate() {
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

// ---- BT connect / disconnect --------------------------------
static void btEvent(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  if (event == ESP_SPP_SRV_OPEN_EVT)   { btConnected = true;  drain(Rs232);
                                         LOG("[bt] connected\n"); }
  else if (event == ESP_SPP_CLOSE_EVT) { btConnected = false; LOG("[bt] closed\n"); }
}

// ---- setup / loop ------------------------------------------
void setup() {
  pinMode(ACT_LED_PIN, OUTPUT);   digitalWrite(ACT_LED_PIN, LOW);
  pinMode(SD_CS_PIN, OUTPUT);     digitalWrite(SD_CS_PIN, HIGH);
  pinMode(TOUCH_CS_PIN, OUTPUT);  digitalWrite(TOUCH_CS_PIN, HIGH);
  pinMode(BTN_SYNC_PIN, INPUT);   // external 10k pull-up on the board
  attachInterrupt(digitalPinToInterrupt(BTN_SYNC_PIN), btnIsr, FALLING);

#if defined(DEBUG_LOG) && DEBUG_LOG
  Serial.begin(DEBUG_BAUD);
#endif

  Rs232.setRxBufferSize(UART_RX_BUFFER);
  Rs232.begin(BRIDGE_BAUD, SERIAL_8N1, RS232_RX_PIN, RS232_TX_PIN);
  GpsSer.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, -1);   // receive-only

  tft.init(240, 320);
  tft.setSPISpeed(TFT_HZ);
  tft.invertDisplay(TFT_INVERT);
  tft.setRotation(1);
  LOG("[tft] %d x %d\n", tft.width(), tft.height());
  staticLayout();
  header();
  footer();

  Bt.register_callback(btEvent);
  Bt.begin(BT_NAME);
#ifdef BT_PIN
  Bt.setPin(BT_PIN, strlen(BT_PIN));
#endif

  LOG("\n[bridge] %s  BT-SPP <-> RS232 @ %d 8N1 (RX=%d TX=%d)  GPS RX=%d  BTN=%d\n",
      BT_NAME, (int)BRIDGE_BAUD, RS232_RX_PIN, RS232_TX_PIN, GPS_RX_PIN, BTN_SYNC_PIN);

  for (int i = 0; i < 3; i++) {
    digitalWrite(ACT_LED_PIN, HIGH); delay(60);
    digitalWrite(ACT_LED_PIN, LOW);  delay(60);
  }
}

void loop() {
  feedGps();
  checkButton();
  static uint32_t syncMs = 0;
  if (millis() - syncMs > 500) { syncMs = millis(); autoSyncTick(); }

  if (btConnected) {
    pump(Bt, Rs232, false);          // client -> mount
    pump(Rs232, Bt, true);           // mount  -> client
  } else {
    drain(Rs232);
    if (millis() - pollLastMs >= POLL_PERIOD_MS) { pollLastMs = millis(); pollOnce(); }
  }

  // LED2 (GPIO2) = mount link: solid ON while the mount is answering over RJ9,
  // OFF within a couple of seconds of the cable being pulled / mount powered off.
  linkUp = mountLinkUp();
  digitalWrite(ACT_LED_PIN, linkUp ? HIGH : LOW);

  static uint32_t uiMs = 0;
  if (millis() - uiMs > 250) { uiMs = millis(); tftUpdate(); }

#if defined(DEBUG_LOG) && DEBUG_LOG
  static uint32_t dbgMs = 0;
  if (millis() - dbgMs > 3000) {
    dbgMs = millis();
    char gs[32]; gpsStatusStr(gs, sizeof gs);
    LOG("[state] %s(%s) link=%s | %s %s | lon %s lat %s | %s %s | RA %s Dec %s | "
        "GPS %s | BTN39=%d BTN36=%d | %s\n",
        M.modelName, M.model, linkUp ? "UP" : "down", M.dateStr, M.timeStr,
        M.lonStr, M.latStr, M.statusStr, M.slewRate,
        M.raStr, M.decStr, gs, digitalRead(39), digitalRead(36),
        btConnected ? "BT-client" : "polling");
  }
#endif

  if (!btConnected) delay(2);
}
