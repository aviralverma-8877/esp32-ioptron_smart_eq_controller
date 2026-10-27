// sky.h - sky database (SD card, or on-chip LittleFS - see SKY_STORAGE_LITTLEFS
// below): card hot-plug (SD only), catalog, per-object info text and thumbnails,
// plus the small amount of astronomy needed for GoTo.
//
// File layout, either backend (SD: built by tools/sdcard/build_sd.py, 8.3 names;
// LittleFS: built by tools/littlefs/build_littlefs.py from the SD card's own data):
//   /SMARTEQ/CATALOG.CSV     '#' comment lines, then
//                            key,label,name,type,con,mag,size_arcmin,ra_hours_j2000,dec_deg_j2000
//                            e.g. M031,M31,Andromeda Galaxy,Galaxy,And,3.4,177.8,0.712319,41.26906
//                            mag / size may be empty. key 'Mnnn' = Messier, 'Snnn' = star,
//                            'Pnnn' = planet / Moon (ra/dec empty: nnn-1 = ephem.h Body).
//   /SMARTEQ/IMG/<key>.565   "R565" + u16 w + u16 h (LE) + w*h little-endian RGB565, row-major
//   /SMARTEQ/INFO/<key>.TXT  CRLF lines: description / "Scope: .." / "Camera: .." / "Exposure: .."
#pragma once
#include <stdint.h>
#include <stddef.h>

// Storage backend (set by platformio.ini build_flags):
//   default            SD card, hot-pluggable, 96x96 thumbnails (see tools/sdcard/)
//   -DSKY_STORAGE_LITTLEFS=1   on-chip LittleFS, always mounted, 64x64 thumbnails
//                       to fit the partition (see tools/littlefs/, partitions_littlefs.csv)
#if defined(SKY_STORAGE_LITTLEFS)
#define SKY_THUMB_PX   64
#define SKY_BADGE      "FS"
#define SKY_NO_CAT_MSG "no catalog data"
#define SKY_LOST_MSG   "catalog data error"
#else
#define SKY_THUMB_PX   96
#define SKY_BADGE      "SD"
#define SKY_NO_CAT_MSG "no SD catalog"
#define SKY_LOST_MSG   "SD card removed"
#endif

enum SkyCat : uint8_t { CAT_MESSIER = 0, CAT_STAR = 1, CAT_PLANET = 2, CAT_COUNT };

struct SkyObj {
  char    key[6];       // "M031" - also the image / info file basename
  char    label[16];    // "M31", "Sirius"
  char    name[28];     // "Andromeda Galaxy", "alf CMa"
  char    type[11];     // "Galaxy", "Open cl.", "Star" ...
  char    con[4];       // "And"
  int16_t mag10;        // V mag x10, INT16_MIN if unknown
  int16_t size10;       // major axis arcmin x10, 0 if unknown
  float   raH;          // J2000 RA, hours
  float   decD;         // J2000 Dec, degrees
  SkyCat  cat;
};

// ---- card + catalog --------------------------------------------------------
void skyBegin();                     // SD: SPI must already be running (displayBegin())
void skyTick();                      // SD: hot-plug (retry mount / detect removal); LittleFS: retry mount once
bool skyReady();                     // card mounted AND catalog loaded (>0 objects)
int  skyCount(SkyCat c);             // objects in a category (catalog file order)
const SkyObj *skyGet(SkyCat c, int i);   // nullptr if out of range

// Reads /SMARTEQ/INFO/<key>.TXT into up to maxLines NUL-terminated lines of
// lineLen bytes each. Returns the number of lines read (0 if missing).
int  skyReadInfo(const SkyObj *o, char lines[][200], int maxLines);

// Streams /SMARTEQ/IMG/<key>.565 to the TFT at (x, y). Returns false (and draws
// nothing) if the file is missing or malformed. Implemented in sky.cpp using
// the shared `tft` object from display.h.
bool skyDrawImage(const SkyObj *o, int16_t x, int16_t y);

// ---- astronomy ------------------------------------------------------------
// Current UTC as a Julian Date. Source: GPS when locked, else the mount's
// own clock (:GLT# local time minus its UTC offset / DST). False if neither.
bool skyNowJD(double &jd);
// Observer site in degrees (east-positive lon). GPS when locked, else mount.
bool skySite(double &latDeg, double &lonDeg);
// J2000 -> mean equinox of date (IAU 1976 precession). Mount GoTo uses JNow.
void skyPrecess(double raH2000, double decD2000, double jd, double &raH, double &decD);
// Position of an object at `jd`, mean equinox of date (JNow - what GoTo needs):
// precessed catalog position, or the ephemeris for planets / the Moon.
bool skyObjNow(const SkyObj *o, double jd, double &raH, double &decD);
// Angular distance (deg) between the object and the Sun right now. GoTo safety.
bool skySunSep(const SkyObj *o, double &sepDeg);
// Altitude/azimuth (degrees, az from N through E) of an object right now.
// False if time or site is unknown.
bool skyAltAz(const SkyObj *o, double &altDeg, double &azDeg);
// Same for a JNow RA (hours) / Dec (degrees), e.g. the mount's own position.
bool skyRaDecToAltAz(double raH, double decD, double &altDeg, double &azDeg);
