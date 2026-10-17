// timeutil.h - civil date <-> days since 1970-01-01 (Howard Hinnant's algorithms).
// Proleptic Gregorian, valid for any year; used for UTC/local epoch maths.
#pragma once

static inline long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  int  yoe = y - (int)(era * 400);
  int  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static inline void civilFromDays(long z, int &y, int &m, int &d) {
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
