#include "test_common.h"
#define HARDSCAN_IMPLEMENTATION
#include "util/hardscan.h"

int main(void)
{
    Hardscan_Report r;

    print_cstr("test_hardscan\n");

    /* no disk_probe_dir: disk probe must be skipped cleanly, everything
     * else still runs */
    hardscan_run(&r, NULL);

    TEST_CHECK(r.n_cpus >= 1, "cpu probe: at least 1 online CPU is always reported");
    TEST_CHECK(r.disk_probe_ran == 0, "disk probe: skipped entirely when disk_probe_dir is NULL");
    TEST_CHECK(r.disk_write_mb_per_sec == 0.0 && r.disk_read_mb_per_sec == 0.0, "disk probe: skipped means both throughput numbers stay at 0");
    TEST_CHECK(r.mem_bandwidth_mb_per_sec > 0.0, "memory probe: a real measured buffer touch always reports positive bandwidth");
    TEST_CHECK(r.recommended_arena_chunk_size >= 4 * 1024 && r.recommended_arena_chunk_size <= 4 * 1024 * 1024,
               "recommended chunk size: always lands inside the documented [4 KiB, 4 MiB] clamp range");

    if (r.l1_dcache_line_size > 0) {
        TEST_CHECK(r.recommended_arena_chunk_size % r.l1_dcache_line_size == 0,
                   "recommended chunk size: is a whole multiple of the detected cache line size");
    }

    /* with a real disk_probe_dir: the write+read round trip must
     * actually happen and report real numbers, and must not leave its
     * temp file behind */
    {
        i32 leftover;

        hardscan_run(&r, "/tmp");

        TEST_CHECK(r.disk_probe_ran == 1, "disk probe: runs when a real writable dir is given");
        TEST_CHECK(r.disk_write_mb_per_sec > 0.0, "disk probe: write throughput measured as positive");
        TEST_CHECK(r.disk_read_mb_per_sec > 0.0, "disk probe: read throughput measured as positive");

        leftover = system("ls /tmp/hardscan_probe_*.tmp >/dev/null 2>&1") == 0;
        TEST_CHECK(leftover == 0, "disk probe: its own temp file is removed afterward, none left behind");
    }

    print_flush();
    return test_failures_ != 0;
}
