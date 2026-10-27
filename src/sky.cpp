// sky.cpp - sky database: catalog, info text, RGB565 thumbnails - on an SD card
// (hot-plug) or on-chip LittleFS (SKY_STORAGE_LITTLEFS, always mounted) - plus
// the astronomy needed for GoTo: JD now, site, IAU 1976 precession, alt/az.
#include "sky.h"
#include "ephem.h"
#include "config.h"
#include "display.h"
#include "mount.h"
#include "gps.h"
#include "bt_bridge.h"
#include "timeutil.h"
#include <SPI.h>
#include <FS.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#if defined(SKY_STORAGE_LITTLEFS)
#include <LittleFS.h>
#define SKY_FS LittleFS
#else
#include <SD.h>
#define SKY_FS SD
#endif

static const char *CAT_PATH  = "/SMARTEQ/CATALOG.CSV";
static const int   CAP       = 400;          // max objects held in RAM (~25 KB)
static const uint32_t RETRY_MS = 4000;       // not mounted: retry SD.begin + load
static const uint32_t CHECK_MS = 3000;       // mounted: probe for card removal

static SkyObj  *objs = nullptr;              // one heap block, file order
static int16_t *idx[CAT_COUNT] = {};
static int      nObj = 0, nCat[CAT_COUNT] = {};
static bool     mounted = false, ready = false;
static int8_t   lastErr = -1;                // 0 ok, 1 no card, 2 bad catalog: log on change only
static uint32_t lastTickMs = 0;

// ---- small helpers --------------------------------------------------------
static void copyTrunc(char *dst, size_t sz, const char *src) {
  size_t n = strlen(src);
  if (n >= sz) n = sz - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

// Split `s` in place on ',' keeping empty fields. Returns field count.
static int splitCsv(char *s, char **f, int maxF) {
  int n = 0;
  f[n++] = s;
  for (char *p = s; *p && n < maxF; p++)
    if (*p == ',') { *p = 0; f[n++] = p + 1; }
  return n;
}

// Buffered line reader (SD byte reads are slow). Strips CR/LF; long lines are
// truncated to the caller's buffer, the rest of the line is discarded.
static uint8_t rdBuf[512];
static int     rdPos = 0, rdLen = 0;
static void rdReset() { rdPos = rdLen = 0; }
static int  rdByte(File &f) {
  if (rdPos >= rdLen) {
    rdLen = f.read(rdBuf, sizeof(rdBuf));
    rdPos = 0;
    if (rdLen <= 0) { rdLen = 0; return -1; }
  }
  return rdBuf[rdPos++];
}
// Returns false at EOF with nothing read.
static bool readLine(File &f, char *out, size_t sz) {
  size_t n = 0;
  int c;
  bool any = false;
  while ((c = rdByte(f)) >= 0) {
    any = true;
    if (c == '\n') break;
    if (c == '\r') continue;
    if (n < sz - 1) out[n++] = (char)c;
  }
  out[n] = 0;
  return any;
}

// ---- catalog --------------------------------------------------------------
static void freeCatalog() {
  free(objs); objs = nullptr;
  for (int c = 0; c < CAT_COUNT; c++) { free(idx[c]); idx[c] = nullptr; nCat[c] = 0; }
  nObj = 0;
  ready = false;
}

static bool loadCatalog() {
  freeCatalog();
  File f = SKY_FS.open(CAT_PATH, FILE_READ);
  if (!f) return false;
  objs = (SkyObj *)malloc(sizeof(SkyObj) * CAP);
  for (int c = 0; c < CAT_COUNT; c++) idx[c] = (int16_t *)malloc(sizeof(int16_t) * CAP);
  if (!objs || !idx[0] || !idx[1]) { f.close(); freeCatalog(); LOG("[sky] no RAM for catalog\n"); return false; }

  static char line[160];
  char *fld[10];
  rdReset();
  while (nObj < CAP && readLine(f, line, sizeof(line))) {
    if (!line[0] || line[0] == '#') continue;
    if (splitCsv(line, fld, 10) < 9) continue;
    SkyCat cat;
    if (fld[0][0] == 'M') cat = CAT_MESSIER;
    else if (fld[0][0] == 'S') cat = CAT_STAR;
    else if (fld[0][0] == 'P') cat = CAT_PLANET;      // position from ephem.cpp
    else continue;
    SkyObj &o = objs[nObj];
    copyTrunc(o.key,   sizeof(o.key),   fld[0]);
    copyTrunc(o.label, sizeof(o.label), fld[1]);
    copyTrunc(o.name,  sizeof(o.name),  fld[2]);
    copyTrunc(o.type,  sizeof(o.type),  fld[3]);
    copyTrunc(o.con,   sizeof(o.con),   fld[4]);
    o.mag10  = fld[5][0] ? (int16_t)lroundf(strtof(fld[5], nullptr) * 10.0f) : INT16_MIN;
    o.size10 = fld[6][0] ? (int16_t)lroundf(strtof(fld[6], nullptr) * 10.0f) : 0;
    o.raH    = strtof(fld[7], nullptr);
    o.decD   = strtof(fld[8], nullptr);
    o.cat    = cat;
    idx[cat][nCat[cat]++] = (int16_t)nObj;
    nObj++;
  }
  f.close();
  ready = nObj > 0;
  if (ready) LOG("[sky] catalog: %d objects (%d Messier, %d stars, %d planets)\n", nObj,
                 nCat[CAT_MESSIER], nCat[CAT_STAR], nCat[CAT_PLANET]);
  return ready;
}

#if defined(SKY_STORAGE_LITTLEFS)
// On-chip flash, always present - mounted once, no hot-plug. Partition built
// by tools/littlefs/build_littlefs.py + `pio run -e esp32dev-littlefs -t uploadfs`.
static bool tryMount() {
  if (!LittleFS.begin(false)) {
    if (lastErr != 1) LOG("[sky] LittleFS mount failed - flash the catalog data (uploadfs)\n");
    lastErr = 1;
    return false;
  }
  mounted = true;
  if (!loadCatalog()) {
    if (lastErr != 2) LOG("[sky] LittleFS mounted but %s missing/empty\n", CAT_PATH);
    lastErr = 2;
    return false;
  }
  lastErr = 0;
  LOG("[sky] LittleFS mounted, %u/%u KB used\n",
      (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
  return true;
}
#else
// SD.begin with no card costs a few hundred ms (CMD0 retries) - only call when idle.
static bool tryMount() {
  if (!SD.begin(SD_CS_PIN, SPI, SD_HZ, "/sd", 5)) {
    if (lastErr != 1) LOG("[sky] SD mount failed (no card?) - retrying every %lus\n", (unsigned long)(RETRY_MS / 1000));
    lastErr = 1;
    return false;
  }
  mounted = true;
  if (!loadCatalog()) {
    if (lastErr != 2) LOG("[sky] SD mounted but %s missing/empty\n", CAT_PATH);
    lastErr = 2;
    SD.end(); mounted = false;
    return false;
  }
  lastErr = 0;
  LOG("[sky] SD mounted, %llu MB\n", (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
  return true;
}
#endif

void skyBegin() {
  tryMount();
  lastTickMs = millis();
#if defined(DEBUG_LOG) && DEBUG_LOG
  // self-check: Polaris (2.530301h, +89.264109) J2000 -> 2026.0 should be
  // RA 3.0975h, Dec +89.3716 (precession is large near the pole).
  double ra, dec;
  skyPrecess(2.530301, 89.264109, 2461041.5, ra, dec);
  LOG("[sky] precess check Polaris 2026.0: RA %.4fh Dec %.4f\n", ra, dec);
#endif
}

void skyTick() {
  if (btConnected) return;                   // never stall the bridge
#if defined(SKY_STORAGE_LITTLEFS)
  if (ready) return;                          // nothing to hot-plug - just retry a failed mount
  uint32_t now = millis();
  if (now - lastTickMs < RETRY_MS) return;
  lastTickMs = now;
  tryMount();
#else
  uint32_t now = millis();
  if (now - lastTickMs < (ready ? CHECK_MS : RETRY_MS)) return;
  lastTickMs = now;
  if (!ready) { tryMount(); return; }
  if (!SD.exists(CAT_PATH)) {
    LOG("[sky] SD card removed\n");
    SD.end();
    mounted = false;
    freeCatalog();
    lastErr = -1;
  }
#endif
}

bool skyReady() { return ready; }

int skyCount(SkyCat c) { return (ready && c < CAT_COUNT) ? nCat[c] : 0; }

const SkyObj *skyGet(SkyCat c, int i) {
  if (!ready || c >= CAT_COUNT || i < 0 || i >= nCat[c]) return nullptr;
  return &objs[idx[c][i]];
}

// ---- info text + thumbnails ----------------------------------------------
int skyReadInfo(const SkyObj *o, char lines[][200], int maxLines) {
  if (!ready || !o || maxLines <= 0) return 0;
  char path[32];
  snprintf(path, sizeof(path), "/SMARTEQ/INFO/%s.TXT", o->key);
  File f = SKY_FS.open(path, FILE_READ);
  if (!f) return 0;
  rdReset();
  int n = 0;
  while (n < maxLines && readLine(f, lines[n], 200)) n++;
  f.close();
  return n;
}

bool skyDrawImage(const SkyObj *o, int16_t x, int16_t y) {
  if (!ready || !o) return false;
  char path[32];
  snprintf(path, sizeof(path), "/SMARTEQ/IMG/%s.565", o->key);
  File f = SKY_FS.open(path, FILE_READ);
  if (!f) return false;
  uint8_t hdr[8];
  if (f.read(hdr, 8) != 8 || memcmp(hdr, "R565", 4) != 0) { f.close(); return false; }
  uint16_t w = hdr[4] | (hdr[5] << 8), h = hdr[6] | (hdr[7] << 8);
  if (w < 1 || w > 240 || h < 1 || h > 240 ||
      f.size() < 8 + (size_t)w * h * 2) { f.close(); return false; }
  // SD and TFT share VSPI: read a chunk (no TFT transaction open), then draw it.
  static uint16_t buf[240 * 8];             // 3.75 KB, static (not stack)
  const int rowsPer = 8;
  for (int row = 0; row < h; row += rowsPer) {
    int nr = (h - row < rowsPer) ? (h - row) : rowsPer;
    size_t want = (size_t)w * nr * 2;
    if (f.read((uint8_t *)buf, want) != want) { f.close(); return false; }
    tft.drawRGBBitmap(x, y + row, buf, w, nr);   // LE RGB565 == native uint16_t
  }
  f.close();
  return true;
}

// ---- astronomy --------------------------------------------------------------
static const double D2R = M_PI / 180.0, R2D = 180.0 / M_PI;
static const double AS2R = M_PI / (180.0 * 3600.0);

bool skyNowJD(double &jd) {
  if (gpsLocked() && gps.date.isValid() && gps.time.isValid()) {
    long days = daysFromCivil(gps.date.year(), gps.date.month(), gps.date.day());
    double s = gps.time.hour() * 3600.0 + gps.time.minute() * 60.0 + gps.time.second()
             + gps.time.centisecond() / 100.0 + gps.time.age() / 1000.0;
    jd = (days * 86400.0 + s) / 86400.0 + 2440587.5;
    return true;
  }
  if (M.timeValid) {
    double utc = (double)M.localEpoch + (double)(uint32_t)(millis() - M.localEpochMs) / 1000.0
               - M.offsetMin * 60.0 - M.dstFlag * 3600.0;
    jd = utc / 86400.0 + 2440587.5;
    return true;
  }
  return false;
}

bool skySite(double &latDeg, double &lonDeg) {
  if (gpsLocked() && gps.location.isValid()) {
    latDeg = gps.location.lat();
    lonDeg = gps.location.lng();
    return true;
  }
  if (M.siteValid) {
    lonDeg = M.lonDeg;
    latDeg = M.latDeg;                // :Gt#, arc-seconds, plain signed latitude
    return true;
  }
  return false;
}

void skyPrecess(double raH2000, double decD2000, double jd, double &raH, double &decD) {
  double T = (jd - 2451545.0) / 36525.0, T2 = T * T, T3 = T2 * T;
  double zeta  = (2306.2181 * T + 0.30188 * T2 + 0.017998 * T3) * AS2R;
  double z     = (2306.2181 * T + 1.09468 * T2 + 0.018203 * T3) * AS2R;
  double theta = (2004.3109 * T - 0.42665 * T2 - 0.041833 * T3) * AS2R;
  double a = raH2000 * 15.0 * D2R, d = decD2000 * D2R;
  double cd = cos(d), sd = sin(d), ct = cos(theta), st = sin(theta);
  double caz = cos(a + zeta), saz = sin(a + zeta);
  double A = cd * saz;
  double B = ct * cd * caz - st * sd;
  double C = st * cd * caz + ct * sd;
  double a2 = atan2(A, B) + z;
  double d2;
  if (fabs(C) > 0.99) {                      // near the poles asin loses precision
    d2 = acos(sqrt(A * A + B * B));
    if (C < 0) d2 = -d2;
  } else d2 = asin(C);
  double h = fmod(a2 * R2D / 15.0, 24.0);
  if (h < 0) h += 24.0;
  if (h >= 24.0) h = 0;
  raH = h;
  decD = d2 * R2D;
}

bool skyObjNow(const SkyObj *o, double jd, double &raH, double &decD) {
  if (!o) return false;
  if (o->cat != CAT_PLANET) { skyPrecess(o->raH, o->decD, jd, raH, decD); return true; }
  int b = atoi(o->key + 1) - 1;
  if (b < 0 || b >= B_COUNT) return false;
  double lat = 0, lon = 0;
  bool site = skySite(lat, lon);                      // Moon parallax needs the site
  return ephemRaDec((Body)b, jd, lat, lon, site, raH, decD);
}

bool skySunSep(const SkyObj *o, double &sepDeg) {
  double jd, ra, dec, sra, sdec;
  if (!o || !skyNowJD(jd) || !skyObjNow(o, jd, ra, dec)) return false;
  ephemSunRaDec(jd, sra, sdec);
  double c = sin(dec * D2R) * sin(sdec * D2R) +
             cos(dec * D2R) * cos(sdec * D2R) * cos((ra - sra) * 15.0 * D2R);
  if (c > 1) c = 1; else if (c < -1) c = -1;
  sepDeg = acos(c) * R2D;
  return true;
}

static void eqToHorizon(double jd, double lat, double lon, double ra, double dec,
                        double &altDeg, double &azDeg) {
  double D = jd - 2451545.0, T = D / 36525.0;
  double gmst = 280.46061837 + 360.98564736629 * D + 0.000387933 * T * T - T * T * T / 38710000.0;
  double ha = fmod(gmst + lon - ra * 15.0, 360.0);
  if (ha < 0) ha += 360.0;
  double H = ha * D2R, dr = dec * D2R, ph = lat * D2R;
  double sAlt = sin(dr) * sin(ph) + cos(dr) * cos(ph) * cos(H);
  if (sAlt > 1) sAlt = 1; else if (sAlt < -1) sAlt = -1;
  altDeg = asin(sAlt) * R2D;
  // az from North through East
  double az = atan2(-cos(dr) * sin(H), sin(dr) * cos(ph) - cos(dr) * sin(ph) * cos(H)) * R2D;
  if (az < 0) az += 360.0;
  if (az >= 360.0) az -= 360.0;
  azDeg = az;
}

bool skyAltAz(const SkyObj *o, double &altDeg, double &azDeg) {
  double jd, lat, lon, ra, dec;
  if (!o || !skyNowJD(jd) || !skySite(lat, lon) || !skyObjNow(o, jd, ra, dec)) return false;
  eqToHorizon(jd, lat, lon, ra, dec, altDeg, azDeg);
  return true;
}

bool skyRaDecToAltAz(double raH, double decD, double &altDeg, double &azDeg) {
  double jd, lat, lon;
  if (!skyNowJD(jd) || !skySite(lat, lon)) return false;
  eqToHorizon(jd, lat, lon, raH, decD, altDeg, azDeg);
  return true;
}
