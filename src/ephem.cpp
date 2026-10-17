// ephem.cpp - low-precision planet + Moon positions for GoTo. Mercury, Venus,
// Mars, Neptune and the Earth from the JPL/Standish Keplerian elements (table 1,
// 1800-2050); Jupiter/Saturn/Uranus from Schlyter's elements + perturbations
// (see below). One light-time step, J2000 -> date by IAU 1976 precession. Moon
// from Meeus ch. 47 (full 60+60 term tables) with topocentric parallax. No
// nutation/aberration (<~30"). The Sun is computed ONLY for the safety guard.
//
// Self-test (vs JPL Horizons apparent RA/Dec; we omit nutation, so ~0.1-1'):
//   ephemRaDec(B_JUPITER, 2461330.5 /*2026-10-17 0h UT*/, ...)
//     -> RA 9h39m19.6s Dec +14 42.0'   (Horizons 9h39m19.8s +14 41.9')
//   ephemSunRaDec(2461330.5) -> RA 13.45875h Dec -9.1774 (Horizons err 0.05')
//   Moon, Meeus ex. 47.a (JD 2448724.5 TT): lambda 133.16265 beta -3.22913
//     dist 368409.7 km (book: 133.162655, -3.229126, 368409.7) -> RA 134.6839
//     Dec +13.7694 deg (book apparent, with nutation: 134.6885, +13.7684).
// Errors vs Horizons, 5 dates 1987-2047: Merc/Ven/Mars <0.7', Jup <1.1',
// Sat <1.5', Ura <1.0', Nep <1.0', Moon geo <0.25', Moon topocentric <0.3'.
#include "ephem.h"
#include <math.h>

static const double PI_  = 3.14159265358979323846;
static const double DR   = PI_ / 180.0, RD = 180.0 / PI_;
static const double ASR  = PI_ / (180.0 * 3600.0);
static const double J2K  = 2451545.0;
static const double DT_D = 69.2 / 86400.0;          // TT-UT (2020s), days
static const double EPS0 = 23.43928 * DR;           // J2000 obliquity
static const double C_AUD = 173.1446327;            // light speed, AU/day
static const double AU_KM = 149597870.7;

static double norm360(double x) { x = fmod(x, 360.0); return x < 0 ? x + 360.0 : x; }

// ---- planets: Standish table 1, {value at J2000, rate per century} --------
// a(AU), e, I, L, long.peri, long.node (deg). Index 0..6 = Mercury..Neptune,
// 7 = Earth-Moon barycentre.
static const double EL[8][6][2] = {
  {{0.38709927, 0.00000037}, {0.20563593, 0.00001906}, {7.00497902, -0.00594749},
   {252.25032350, 149472.67411175}, {77.45779628, 0.16047689}, {48.33076593, -0.12534081}},
  {{0.72333566, 0.00000390}, {0.00677672, -0.00004107}, {3.39467605, -0.00078890},
   {181.97909950, 58517.81538729}, {131.60246718, 0.00268329}, {76.67984255, -0.27769418}},
  {{1.52371034, 0.00001847}, {0.09339410, 0.00007882}, {1.84969142, -0.00813131},
   {-4.55343205, 19140.30268499}, {-23.94362959, 0.44441088}, {49.55953891, -0.29257343}},
  {{5.20288700, -0.00011607}, {0.04838624, -0.00013253}, {1.30439695, -0.00183714},
   {34.39644051, 3034.74612775}, {14.72847983, 0.21252668}, {100.47390909, 0.20469106}},
  {{9.53667594, -0.00125060}, {0.05386179, -0.00050991}, {2.48599187, 0.00193609},
   {49.95424423, 1222.49362201}, {92.59887831, -0.41897216}, {113.66242448, -0.28867794}},
  {{19.18916464, -0.00196176}, {0.04725744, -0.00004397}, {0.77263783, -0.00242939},
   {313.23810451, 428.48202785}, {170.95427630, 0.40805281}, {74.01692503, 0.04240589}},
  {{30.06992276, 0.00026291}, {0.00859048, 0.00005105}, {1.77004347, 0.00035372},
   {-55.12002969, 218.45945325}, {44.96476227, -0.32241464}, {131.78422574, -0.00508664}},
  {{1.00000261, 0.00000562}, {0.01671123, -0.00004392}, {-0.00001531, -0.01294668},
   {100.46457166, 35999.37244981}, {102.93768193, 0.32327364}, {0.0, 0.0}},
};

// Heliocentric ecliptic J2000 position (AU) of element set p at T (TT centuries).
static void helio(int p, double T, double v[3]) {
  const double (*E)[2] = EL[p];
  double a = E[0][0] + E[0][1] * T, e = E[1][0] + E[1][1] * T;
  double I = (E[2][0] + E[2][1] * T) * DR;
  double L = E[3][0] + E[3][1] * T, wb = E[4][0] + E[4][1] * T;
  double O = (E[5][0] + E[5][1] * T) * DR;
  double w = wb * DR - O;
  double M = norm360(L - wb);
  if (M > 180.0) M -= 360.0;
  M *= DR;
  double Ea = M + e * sin(M);                        // Kepler, Newton steps
  for (int i = 0; i < 6; i++) Ea -= (Ea - e * sin(Ea) - M) / (1.0 - e * cos(Ea));
  double xp = a * (cos(Ea) - e), yp = a * sqrt(1.0 - e * e) * sin(Ea);
  double cw = cos(w), sw = sin(w), cO = cos(O), sO = sin(O), cI = cos(I), sI = sin(I);
  v[0] = (cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp;
  v[1] = (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp;
  v[2] = (sw * sI) * xp + (cw * sI) * yp;
}

// ---- Jupiter, Saturn, Uranus ----------------------------------------------
// Standish table 1 is 5-10' off for Jupiter/Saturn (the mutual perturbations
// are only absorbed into the mean elements), so these three use P. Schlyter's
// elements of date + main perturbation terms instead. Measured vs JPL DE over
// 1985-2055 (geocentric): Jupiter <1.9', Saturn <2.2', Uranus <1.5' (Standish:
// 10.6', 10.4', 3.3'). Saturn/Uranus radius biases fitted to DE.
// {N, i, w, a, e, M} as {value at JD 2451543.5, rate per day}.
static const double SE[3][6][2] = {
  {{100.4542, 2.76854e-5}, {1.3030, -1.557e-7}, {273.8777, 1.64505e-5},
   {5.20256, 0.0}, {0.048498, 4.469e-9}, {19.8950, 0.0830853001}},
  {{113.6634, 2.38980e-5}, {2.4886, -1.081e-7}, {339.3939, 2.97661e-5},
   {9.55475, 0.0}, {0.055546, -9.499e-9}, {316.9670, 0.0334442282}},
  {{74.0005, 1.3978e-5}, {0.7733, 1.9e-8}, {96.6612, 3.0565e-5},
   {19.18171, -1.55e-8}, {0.047318, 7.45e-9}, {142.5905, 0.011725806}},
};

static void helioOuter(int p, double T, double v[3]) {    // p = 3..5
  double d = T * 36525.0 + 1.5;
  const double (*E)[2] = SE[p - 3];
  double N = (E[0][0] + E[0][1] * d) * DR, i = (E[1][0] + E[1][1] * d) * DR;
  double w = (E[2][0] + E[2][1] * d) * DR, a = E[3][0] + E[3][1] * d;
  double e = E[4][0] + E[4][1] * d, M = norm360(E[5][0] + E[5][1] * d) * DR;
  double Mj = norm360(SE[0][5][0] + SE[0][5][1] * d) * DR;
  double Ms = norm360(SE[1][5][0] + SE[1][5][1] * d) * DR;
  double Mu = norm360(SE[2][5][0] + SE[2][5][1] * d) * DR;
  double Ea = M + e * sin(M);
  for (int k = 0; k < 6; k++) Ea -= (Ea - e * sin(Ea) - M) / (1.0 - e * cos(Ea));
  double xv = a * (cos(Ea) - e), yv = a * sqrt(1.0 - e * e) * sin(Ea);
  double r = sqrt(xv * xv + yv * yv), u = atan2(yv, xv) + w;
  double x = r * (cos(N) * cos(u) - sin(N) * sin(u) * cos(i));
  double y = r * (sin(N) * cos(u) + cos(N) * sin(u) * cos(i));
  double z = r * sin(u) * sin(i);
  double lon = atan2(y, x), lat = atan2(z, sqrt(x * x + y * y)), dl = 0, db = 0;
  if (p == 3) {
    dl = -0.332 * sin(2 * Mj - 5 * Ms - 67.6 * DR) - 0.056 * sin(2 * Mj - 2 * Ms + 21 * DR)
       + 0.042 * sin(3 * Mj - 5 * Ms + 21 * DR) - 0.036 * sin(Mj - 2 * Ms)
       + 0.022 * cos(Mj - Ms) + 0.023 * sin(2 * Mj - 3 * Ms + 52 * DR)
       - 0.016 * sin(Mj - 5 * Ms - 69 * DR);
  } else if (p == 4) {
    dl = 0.812 * sin(2 * Mj - 5 * Ms - 67.6 * DR) - 0.229 * cos(2 * Mj - 4 * Ms - 2 * DR)
       + 0.119 * sin(Mj - 2 * Ms - 3 * DR) + 0.046 * sin(2 * Mj - 6 * Ms - 69 * DR)
       + 0.014 * sin(Mj - 3 * Ms + 32 * DR);
    db = -0.020 * cos(2 * Mj - 4 * Ms - 2 * DR) + 0.018 * sin(2 * Mj - 6 * Ms - 49 * DR);
    r += -0.0164 + 0.0085 * sin(Ms) + 0.0159 * cos(Ms);
  } else {
    dl = 0.040 * sin(Ms - 2 * Mu + 6 * DR) + 0.035 * sin(Ms - 3 * Mu + 33 * DR)
       - 0.015 * sin(Mj - Mu + 20 * DR);
    r += 0.0100;
  }
  lon += dl * DR - (1.396971 * T + 0.0003086 * T * T) * DR;  // of date -> J2000
  lat += db * DR;
  v[0] = r * cos(lat) * cos(lon);
  v[1] = r * cos(lat) * sin(lon);
  v[2] = r * sin(lat);
}

static void helioBody(int p, double T, double v[3]) {
  if (p >= 3 && p <= 5) helioOuter(p, T, v); else helio(p, T, v);
}

// ---- Moon: Meeus ch. 47 ---------------------------------------------------
// Longitude/distance terms: D, M, M', F, sum_l (1e-6 deg), sum_r (1e-3 km).
static const int8_t  ML_ARG[60][4] = {
  {0,0,1,0},{2,0,-1,0},{2,0,0,0},{0,0,2,0},{0,1,0,0},{0,0,0,2},{2,0,-2,0},{2,-1,-1,0},
  {2,0,1,0},{2,-1,0,0},{0,1,-1,0},{1,0,0,0},{0,1,1,0},{2,0,0,-2},{0,0,1,2},{0,0,1,-2},
  {4,0,-1,0},{0,0,3,0},{4,0,-2,0},{2,1,-1,0},{2,1,0,0},{1,0,-1,0},{1,1,0,0},{2,-1,1,0},
  {2,0,2,0},{4,0,0,0},{2,0,-3,0},{0,1,-2,0},{2,0,-1,2},{2,-1,-2,0},{1,0,1,0},{2,-2,0,0},
  {0,1,2,0},{0,2,0,0},{2,-2,-1,0},{2,0,1,-2},{2,0,0,2},{4,-1,-1,0},{0,0,2,2},{3,0,-1,0},
  {2,1,1,0},{4,-1,-2,0},{0,2,-1,0},{2,2,-1,0},{2,1,-2,0},{2,-1,0,-2},{4,0,1,0},{0,0,4,0},
  {4,-1,0,0},{1,0,-2,0},{2,1,0,-2},{0,0,2,-2},{1,1,1,0},{3,0,-2,0},{4,0,-3,0},{2,-1,2,0},
  {0,2,1,0},{1,1,-1,0},{2,0,3,0},{2,0,-1,-2}};
static const int32_t ML_L[60] = {
  6288774,1274027,658314,213618,-185116,-114332,58793,57066,53322,45758,-40923,-34720,
  -30383,15327,-12528,10980,10675,10034,8548,-7888,-6766,-5163,4987,4036,3994,3861,3665,
  -2689,-2602,2390,-2348,2236,-2120,-2069,2048,-1773,-1595,1215,-1110,-892,-810,759,-713,
  -700,691,596,549,537,520,-487,-399,-381,351,-340,330,327,-323,299,294,0};
static const int32_t ML_R[60] = {
  -20905355,-3699111,-2955968,-569925,48888,-3149,246158,-152138,-170733,-204586,
  -129620,108743,104755,10321,0,79661,-34782,-23210,-21636,24208,30824,-8379,-16675,
  -12831,-10445,-11650,14403,-7003,0,10056,6322,-9884,5751,0,-4950,4130,0,-3958,0,3258,
  2616,-1897,-2117,2354,0,0,-1423,-1117,-1571,-1739,0,-4421,0,0,0,0,1165,0,0,8752};
// Latitude terms: D, M, M', F, sum_b (1e-6 deg).
static const int8_t  MB_ARG[60][4] = {
  {0,0,0,1},{0,0,1,1},{0,0,1,-1},{2,0,0,-1},{2,0,-1,1},{2,0,-1,-1},{2,0,0,1},{0,0,2,1},
  {2,0,1,-1},{0,0,2,-1},{2,-1,0,-1},{2,0,-2,-1},{2,0,1,1},{2,1,0,-1},{2,-1,-1,1},{2,-1,0,1},
  {2,-1,-1,-1},{0,1,-1,-1},{4,0,-1,-1},{0,1,0,1},{0,0,0,3},{0,1,-1,1},{1,0,0,1},{0,1,1,1},
  {0,1,1,-1},{0,1,0,-1},{1,0,0,-1},{0,0,3,1},{4,0,0,-1},{4,0,-1,1},{0,0,1,-3},{4,0,-2,1},
  {2,0,0,-3},{2,0,2,-1},{2,-1,1,-1},{2,0,-2,1},{0,0,3,-1},{2,0,2,1},{2,0,-3,-1},{2,1,-1,1},
  {2,1,0,1},{4,0,0,1},{2,-1,1,1},{2,-2,0,-1},{0,0,1,3},{2,1,1,-1},{1,1,0,-1},{1,1,0,1},
  {0,1,-2,-1},{2,1,-1,-1},{1,0,1,1},{2,-1,-2,-1},{0,1,2,1},{4,0,-2,-1},{4,-1,-1,-1},
  {1,0,1,-1},{4,0,1,-1},{1,0,-1,-1},{4,-1,0,-1},{2,-2,0,1}};
static const int32_t MB_B[60] = {
  5128122,280602,277693,173237,55413,46271,32573,17198,9266,8822,8216,4324,4200,-3359,
  2463,2211,2065,-1870,1828,-1794,-1749,-1565,-1491,-1475,-1410,-1344,-1335,1107,1021,
  833,777,671,607,596,491,-451,439,422,421,-366,-351,331,315,302,-283,-229,223,223,-220,
  -220,-185,181,-177,176,166,-164,132,-119,115,107};

// Geocentric ecliptic lon/lat (deg, mean equinox of date) and distance (km).
static void moonEcl(double T, double &lam, double &bet, double &dist) {
  double T2 = T * T, T3 = T2 * T, T4 = T3 * T;
  double Lp = 218.3164477 + 481267.88123421 * T - 0.0015786 * T2 + T3 / 538841.0 - T4 / 65194000.0;
  double D  = 297.8501921 + 445267.1114034 * T - 0.0018819 * T2 + T3 / 545868.0 - T4 / 113065000.0;
  double M  = 357.5291092 + 35999.0502909 * T - 0.0001536 * T2 + T3 / 24490000.0;
  double Mp = 134.9633964 + 477198.8675055 * T + 0.0087414 * T2 + T3 / 69699.0 - T4 / 14712000.0;
  double F  = 93.2720950 + 483202.0175233 * T - 0.0036539 * T2 - T3 / 3526000.0 + T4 / 863310000.0;
  double A1 = 119.75 + 131.849 * T, A2 = 53.09 + 479264.290 * T, A3 = 313.45 + 481266.484 * T;
  double E = 1.0 - 0.002516 * T - 0.0000074 * T2;
  Lp = norm360(Lp); D = norm360(D) * DR; M = norm360(M) * DR; Mp = norm360(Mp) * DR; F = norm360(F) * DR;
  double sl = 0, sr = 0, sb = 0;
  for (int i = 0; i < 60; i++) {
    const int8_t *g = ML_ARG[i];
    double arg = g[0] * D + g[1] * M + g[2] * Mp + g[3] * F;
    double k = g[1] == 0 ? 1.0 : (g[1] == 1 || g[1] == -1 ? E : E * E);
    sl += k * ML_L[i] * sin(arg);
    sr += k * ML_R[i] * cos(arg);
    g = MB_ARG[i];
    arg = g[0] * D + g[1] * M + g[2] * Mp + g[3] * F;
    k = g[1] == 0 ? 1.0 : (g[1] == 1 || g[1] == -1 ? E : E * E);
    sb += k * MB_B[i] * sin(arg);
  }
  double LpR = Lp * DR;
  sl += 3958 * sin(A1 * DR) + 1962 * sin(LpR - F) + 318 * sin(A2 * DR);
  sb += -2235 * sin(LpR) + 382 * sin(A3 * DR) + 175 * sin(A1 * DR - F) + 175 * sin(A1 * DR + F)
        + 127 * sin(LpR - Mp) - 115 * sin(LpR + Mp);
  lam = norm360(Lp + sl * 1e-6);
  bet = sb * 1e-6;
  dist = 385000.56 + sr * 1e-3;
}

// Earth heliocentric (ecliptic J2000, AU): EMB minus the Moon's share (1/82.3).
// The Moon vector is of-date, but the precession error on a 4700 km offset is nil.
static void earthHelio(double T, double v[3]) {
  helio(7, T, v);
  double lam, bet, dist;
  moonEcl(T, lam, bet, dist);
  double k = dist / AU_KM / 82.30057;
  double cb = cos(bet * DR);
  v[0] -= k * cb * cos(lam * DR);
  v[1] -= k * cb * sin(lam * DR);
  v[2] -= k * sin(bet * DR);
}

// Geocentric ecliptic J2000 vector of planet p (light-time corrected) plus the
// Sun-planet (r), Sun-Earth (R) and Earth-planet (delta) distances in AU.
static void planetGeo(int p, double jdTT, double g[3], double &r, double &R, double &delta) {
  double T = (jdTT - J2K) / 36525.0, e[3], h[3];
  earthHelio(T, e);
  helioBody(p, T, h);
  for (int k = 0; k < 3; k++) g[k] = h[k] - e[k];
  double tau = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]) / C_AUD;
  helioBody(p, T - tau / 36525.0, h);                    // one light-time step
  for (int k = 0; k < 3; k++) g[k] = h[k] - e[k];
  r = sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
  R = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
  delta = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
}

// IAU 1976 precession J2000 -> mean equinox of date (radians in/out).
static void precess(double a, double d, double jd, double &a2, double &d2) {
  double T = (jd - J2K) / 36525.0, T2 = T * T, T3 = T2 * T;
  double zeta  = (2306.2181 * T + 0.30188 * T2 + 0.017998 * T3) * ASR;
  double z     = (2306.2181 * T + 1.09468 * T2 + 0.018203 * T3) * ASR;
  double theta = (2004.3109 * T - 0.42665 * T2 - 0.041833 * T3) * ASR;
  double cd = cos(d), sd = sin(d), ct = cos(theta), st = sin(theta);
  double caz = cos(a + zeta), saz = sin(a + zeta);
  double A = cd * saz, B = ct * cd * caz - st * sd, C = st * cd * caz + ct * sd;
  a2 = atan2(A, B) + z;
  d2 = atan2(C, sqrt(A * A + B * B));
}

static double raHours(double aRad) {
  double h = fmod(aRad * RD / 15.0, 24.0);
  if (h < 0) h += 24.0;
  if (h >= 24.0) h = 0;
  return h;
}

// Ecliptic (lon/lat rad) -> equatorial (ra/dec rad) for obliquity eps.
static void ecl2eq(double l, double b, double eps, double &a, double &d) {
  double ce = cos(eps), se = sin(eps);
  a = atan2(sin(l) * ce - tan(b) * se, cos(l));
  d = asin(sin(b) * ce + cos(b) * se * sin(l));
}

// Moon RA/Dec of date (rad), geocentric, plus distance (km).
static void moonEq(double jdTT, double &a, double &d, double &dist) {
  double T = (jdTT - J2K) / 36525.0, lam, bet;
  moonEcl(T, lam, bet, dist);
  double eps = (23.0 + 26.0 / 60.0 + 21.448 / 3600.0) * DR                 // Meeus 22.2
             + (-46.8150 * T - 0.00059 * T * T + 0.001813 * T * T * T) * ASR;
  ecl2eq(lam * DR, bet * DR, eps, a, d);
}

bool ephemRaDec(Body b, double jd, double latDeg, double lonDeg, bool siteKnown,
                double &raH, double &decD) {
  if (b >= B_COUNT) return false;
  double jdTT = jd + DT_D, a, d;
  if (b == B_MOON) {
    double dist;
    moonEq(jdTT, a, d, dist);
    if (siteKnown) {                                 // Meeus ch. 40, spherical-ish Earth
      double D = jd - J2K, T = D / 36525.0;
      double gmst = 280.46061837 + 360.98564736629 * D + 0.000387933 * T * T - T * T * T / 38710000.0;
      double H = norm360(gmst + lonDeg) * DR - a;
      double ph = latDeg * DR, rc = cos(ph), rs = 0.99664719 * sin(ph);
      double sp = 6378.14 / dist;                    // sin(horizontal parallax)
      double den = cos(d) - rc * sp * cos(H);
      double da = atan2(-rc * sp * sin(H), den);
      d = atan2((sin(d) - rs * sp) * cos(da), den);
      a += da;
    }
  } else {
    double g[3], r, R, del;
    planetGeo(b, jdTT, g, r, R, del);
    double ce = cos(EPS0), se = sin(EPS0);           // ecliptic -> equatorial J2000
    double x = g[0], y = g[1] * ce - g[2] * se, z = g[1] * se + g[2] * ce;
    precess(atan2(y, x), atan2(z, sqrt(x * x + y * y)), jdTT, a, d);
  }
  raH = raHours(a);
  decD = d * RD;
  return true;
}

// Safety guard only (UI refuses GoTo near the Sun) - never a GoTo target.
// Geocentric Sun = -Earth heliocentric, light-time (~8 min, 20") ignored.
void ephemSunRaDec(double jd, double &raH, double &decD) {
  double jdTT = jd + DT_D, e[3], a, d;
  earthHelio((jdTT - J2K) / 36525.0, e);
  double ce = cos(EPS0), se = sin(EPS0);
  double x = -e[0], y = -e[1] * ce + e[2] * se, z = -e[1] * se - e[2] * ce;
  precess(atan2(y, x), atan2(z, sqrt(x * x + y * y)), jdTT, a, d);
  raH = raHours(a);
  decD = d * RD;
}

float ephemMag(Body b, double jd) {
  if (b >= B_COUNT) return 99.0f;
  double jdTT = jd + DT_D;
  if (b == B_MOON) {
    // phase angle from the Moon's elongation from the Sun (Sun = -Earth helio)
    double T = (jdTT - J2K) / 36525.0, e[3], lam, bet, dist;
    earthHelio(T, e);
    moonEcl(T, lam, bet, dist);
    double R = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    double ls = atan2(-e[1], -e[0]) + (1.3972 * T) * DR;       // J2000 -> of date (approx)
    double cpsi = cos(bet * DR) * cos(lam * DR - ls);
    double psi = acos(cpsi), dAU = dist / AU_KM;
    double i = atan2(R * sin(psi), dAU - R * cpsi) * RD;
    return (float)(-12.7 + 0.026 * fabs(i) + 4e-9 * i * i * i * i + 5.0 * log10(dist / 384400.0));
  }
  double g[3], r, R, del;
  planetGeo(b, jdTT, g, r, R, del);
  double ci = (r * r + del * del - R * R) / (2.0 * r * del);
  if (ci > 1) ci = 1; else if (ci < -1) ci = -1;
  double i = acos(ci) * RD, m = 5.0 * log10(r * del);
  switch (b) {                                       // Explanatory Supplement (1992)
    case B_MERCURY: m += -0.42 + 0.0380 * i - 0.000273 * i * i + 0.000002 * i * i * i; break;
    case B_VENUS:   m += -4.40 + 0.0009 * i + 0.000239 * i * i - 0.00000065 * i * i * i; break;
    case B_MARS:    m += -1.52 + 0.016 * i; break;
    case B_JUPITER: m += -9.40 + 0.005 * i; break;
    case B_SATURN: {
      // ring tilt B as seen from Earth: ring pole RA 40.589, Dec 83.537 (J2000)
      double ce = cos(EPS0), se = sin(EPS0);
      double x = g[0], y = g[1] * ce - g[2] * se, z = g[1] * se + g[2] * ce;
      double pa = 40.589 * DR, pd = 83.537 * DR;
      double sB = -(x * cos(pd) * cos(pa) + y * cos(pd) * sin(pa) + z * sin(pd)) / del;
      m += -8.88 + 0.044 * i - 2.60 * fabs(sB) + 1.25 * sB * sB;
      break;
    }
    case B_URANUS:  m += -7.19 + 0.002 * i; break;
    case B_NEPTUNE: m += -7.00; break;
    default: break;
  }
  return (float)m;
}
