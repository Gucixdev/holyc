#ifndef HOLYC_DATE_UTIL_H_
#define HOLYC_DATE_UTIL_H_

/* Calendar-date arithmetic via the Julian Day Number (JDN), NOT via
 * <time.h>'s struct tm/mktime - mktime is timezone- and DST-aware by
 * design (it interprets a struct tm in the LOCAL timezone), which is
 * exactly the wrong tool for pure calendar math like "what's 45 days
 * after 2024-03-01" or "what weekday was 1776-07-04" - those are
 * timezone-independent questions and mktime can silently answer a
 * different question (a local-time edge case around a DST transition)
 * if handed one. The JDN is just an integer day count with no timezone
 * concept at all, so it sidesteps that whole footgun class.
 *
 * date_to_jdn/jdn_to_date implement the standard Fliegel & Van Flandern
 * algorithm (the same one used by the Naval Observatory / most JDN
 * reference implementations) - real, published, not improvised. Verified
 * in tests/test_date_util.c against the commonly-cited anchor
 * 2000-01-01 == JDN 2451545, plus an exhaustive round-trip sweep.
 *
 * day_of_week uses JDN % 7 with the 0=Monday convention (verified
 * against 2000-01-01, a known Saturday, in the same test). */

#include "base.h"

typedef struct {
    i32 year;
    i32 month; /* 1-12 */
    i32 day;   /* 1-31 */
} Date;

i64 date_to_jdn(Date d);
Date jdn_to_date(i64 jdn);

/* 0=Monday .. 6=Sunday */
i32 date_day_of_week(Date d);

Date date_add_days(Date d, i64 days);
i64 date_diff_days(Date a, Date b); /* a - b, in days */

#endif /* HOLYC_DATE_UTIL_H_ */

#ifdef DATE_UTIL_IMPLEMENTATION
#ifndef DATE_UTIL_IMPLEMENTATION_
#define DATE_UTIL_IMPLEMENTATION_

i64 date_to_jdn(Date d)
{
    i64 y = d.year;
    i64 m = d.month;
    i64 day = d.day;
    i64 a = (m - 14) / 12;
    i64 jdn;

    jdn = (1461 * (y + 4800 + a)) / 4
        + (367 * (m - 2 - 12 * a)) / 12
        - (3 * ((y + 4900 + a) / 100)) / 4
        + day - 32075;

    return jdn;
}

Date jdn_to_date(i64 jdn)
{
    i64 l, n, i, j;
    Date d;

    l = jdn + 68569;
    n = (4 * l) / 146097;
    l = l - (146097 * n + 3) / 4;
    i = (4000 * (l + 1)) / 1461001;
    l = l - (1461 * i) / 4 + 31;
    j = (80 * l) / 2447;

    d.day = (i32)(l - (2447 * j) / 80);
    l = j / 11;
    d.month = (i32)(j + 2 - 12 * l);
    d.year = (i32)(100 * (n - 49) + i + l);

    return d;
}

i32 date_day_of_week(Date d)
{
    i64 jdn = date_to_jdn(d);
    i64 dow = jdn % 7;
    if (dow < 0) dow += 7; /* defensive: only matters for jdn < 0, i.e. dates before -4712-01-01 */
    return (i32)dow;
}

Date date_add_days(Date d, i64 days)
{
    return jdn_to_date(date_to_jdn(d) + days);
}

i64 date_diff_days(Date a, Date b)
{
    return date_to_jdn(a) - date_to_jdn(b);
}

#endif /* DATE_UTIL_IMPLEMENTATION_ */
#endif /* DATE_UTIL_IMPLEMENTATION */
