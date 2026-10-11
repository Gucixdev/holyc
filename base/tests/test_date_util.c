#include "test_common.h"
#define DATE_UTIL_IMPLEMENTATION
#include "util/date_util.h"

int main(void)
{
    print_cstr("test_date_util\n");

    {
        Date d;
        d.year = 2000; d.month = 1; d.day = 1;
        TEST_CHECK(date_to_jdn(d) == 2451545, "date_to_jdn: 2000-01-01 == JDN 2451545 (well-known reference anchor)");
    }

    {
        Date back = jdn_to_date(2451545);
        TEST_CHECK(back.year == 2000 && back.month == 1 && back.day == 1, "jdn_to_date: inverse of the reference anchor round-trips exactly");
    }

    {
        Date d;
        d.year = 2000; d.month = 1; d.day = 1;
        /* 2000-01-01 is a well-known Saturday. 0=Monday..6=Sunday, so Saturday == 5. */
        TEST_CHECK(date_day_of_week(d) == 5, "day_of_week: 2000-01-01 was a Saturday");
    }

    {
        Date d;
        d.year = 2024; d.month = 3; d.day = 1;
        /* 2024-03-01 was a Friday. */
        TEST_CHECK(date_day_of_week(d) == 4, "day_of_week: 2024-03-01 was a Friday");
    }

    {
        /* exhaustive round-trip sweep: every JDN in a wide window converts
         * to a date and back to the exact same JDN - catches any formula
         * transcription bug across a huge range, not just one hand-picked
         * anchor. */
        i64 jdn;
        i32 all_ok = 1;
        for (jdn = 2400000; jdn <= 2500000; jdn++) {
            Date d = jdn_to_date(jdn);
            if (date_to_jdn(d) != jdn) { all_ok = 0; break; }
        }
        TEST_CHECK(all_ok, "date_to_jdn(jdn_to_date(x)) == x across 100001 consecutive days");
    }

    {
        Date d;
        d.year = 2024; d.month = 2; d.day = 28;
        {
            Date after = date_add_days(d, 1);
            TEST_CHECK(after.year == 2024 && after.month == 2 && after.day == 29, "date_add_days: crosses into a leap day correctly (2024 is a leap year)");
        }
        {
            Date after = date_add_days(d, 2);
            TEST_CHECK(after.year == 2024 && after.month == 3 && after.day == 1, "date_add_days: crosses a leap-year month boundary correctly");
        }
    }

    {
        Date a, b;
        a.year = 2024; a.month = 3; a.day = 1;
        b.year = 2024; b.month = 2; b.day = 28;
        TEST_CHECK(date_diff_days(a, b) == 2, "date_diff_days: 2024-03-01 minus 2024-02-28 is 2 (leap day counted)");
        TEST_CHECK(date_diff_days(b, a) == -2, "date_diff_days: reversed order negates cleanly");
    }

    print_flush();
    return test_failures_ != 0;
}
