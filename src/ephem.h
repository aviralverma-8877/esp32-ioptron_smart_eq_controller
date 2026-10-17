// ephem.h - low-precision ephemeris for the planets and the Moon (no Sun: never
// point the mount at it). Good to ~1-2 arcmin for planets, a few arcmin for the
// Moon - plenty for GoTo and altitude checks.
#pragma once
#include <stdint.h>

enum Body : uint8_t {
  B_MERCURY, B_VENUS, B_MARS, B_JUPITER, B_SATURN, B_URANUS, B_NEPTUNE, B_MOON, B_COUNT
};

// Apparent RA/Dec referred to the mean equinox of date (JNow - what the mount
// expects) for a UTC Julian date. Planets: geocentric, light-time corrected.
// Moon: topocentric when siteKnown (parallax is up to ~1 deg), else geocentric.
// Returns false for an invalid body.
bool ephemRaDec(Body b, double jd, double latDeg, double lonDeg, bool siteKnown,
                double &raH, double &decD);

// Approximate visual magnitude right now (for the list / object page).
float ephemMag(Body b, double jd);

// Geocentric Sun RA/Dec (JNow). SAFETY USE ONLY - lets the UI refuse GoTo to
// anything close to the Sun. Never a GoTo target.
void ephemSunRaDec(double jd, double &raH, double &decD);
