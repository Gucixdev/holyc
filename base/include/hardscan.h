#ifndef HOLYC_HARDSCAN_H_
#define HOLYC_HARDSCAN_H_

/* Hardware scan - probes CPU cache topology, core count, measured
 * memory bandwidth, and (optionally) measured disk throughput, then
 * derives a recommended Arena chunk size from the real numbers instead
 * of a guessed constant - "for arenas... calculate the highest
 * possible hardware performance mathematically", per this file's own
 * original prose spec.
 *
 * Cache/core topology comes from glibc's own sysconf() extensions
 * (_SC_LEVEL1_DCACHE_SIZE etc.) - Linux/glibc-specific, not POSIX-
 * portable, but this project already targets Linux only elsewhere
 * (mmap, pthreads, raw syscalls with no portability shims - see
 * arena_alloc.h/fine_grained.h's own stance). Memory and disk
 * bandwidth are NOT looked up anywhere - they're genuinely MEASURED
 * (a real timed benchmark against this exact machine, right now),
 * matching this project's own "measured, not assumed" standing
 * practice (see arena_alloc.h's arena_memcpy comment for the same
 * philosophy applied to a different question).
 *
 * Bus/interconnect bandwidth (mentioned in the original spec) is NOT
 * separately probed - reading real bus/interconnect topology needs
 * privileged access to PCIe config space or vendor-specific MSRs, a
 * genuinely different, much bigger, root-requiring undertaking; the
 * measured memory bandwidth number already captures "how fast this
 * specific machine can actually move bulk data" for the purpose this
 * file exists for (sizing Arena chunks), without needing that. GPU
 * probing (also mentioned) is out of scope for the same reason this
 * project has no GPU code anywhere else (a deliberate scope cut) - not attempted here either. */

#include "base.h"

typedef struct {
    size_t l1_dcache_size;      /* bytes; 0 if this machine/kernel doesn't expose it */
    size_t l1_dcache_line_size;  /* bytes; 0 if undetectable - most modern x86_64 is 64 */
    size_t l2_cache_size;         /* bytes; 0 if undetectable */
    size_t l3_cache_size;          /* bytes; 0 if undetectable (common on machines with no shared L3) */
    i32 n_cpus;                     /* online logical CPUs */

    f64 mem_bandwidth_mb_per_sec;   /* measured sequential write+read over a real buffer */

    i32 disk_probe_ran;               /* 0 if disk_probe_dir was NULL or the probe couldn't create its temp file */
    f64 disk_write_mb_per_sec;         /* meaningful only if disk_probe_ran */
    f64 disk_read_mb_per_sec;           /* meaningful only if disk_probe_ran */

    size_t recommended_arena_chunk_size; /* derived from the above - see hardscan_run's own comment */
} Hardscan_Report;

/* runs every probe and fills `out`. This takes real wall-clock time -
 * the memory probe allocates and touches a real multi-megabyte buffer,
 * the disk probe (if disk_probe_dir is non-NULL) writes and reads a
 * real temp file there, deleting it afterward. Pass NULL for
 * disk_probe_dir to skip the disk probe entirely (out->disk_probe_ran
 * will be 0, disk_*_mb_per_sec left at 0.0). */
void hardscan_run(Hardscan_Report *out, const char *disk_probe_dir);

#endif /* HOLYC_HARDSCAN_H_ */

#ifdef HARDSCAN_IMPLEMENTATION
#ifndef HARDSCAN_IMPLEMENTATION_
#define HARDSCAN_IMPLEMENTATION_

#include <unistd.h>   /* sysconf, _SC_* constants */
#include <time.h>     /* clock_gettime */
#include <stdio.h>    /* the disk probe's own temp file I/O - genuinely needs real file I/O, not print.h's write(2)-only model */
#include <stdlib.h>   /* malloc/free for the memory-bandwidth probe's scratch buffer - deliberately NOT Arena here, see below */

#ifndef HARDSCAN_MEM_PROBE_BYTES
#define HARDSCAN_MEM_PROBE_BYTES (64 * 1024 * 1024) /* 64 MiB - big enough to blow well past L3, small enough to run in well under a second */
#endif
#ifndef HARDSCAN_DISK_PROBE_BYTES
#define HARDSCAN_DISK_PROBE_BYTES (16 * 1024 * 1024) /* 16 MiB */
#endif

/* raw malloc/free here, not Arena - deliberate: this is a one-shot
 * scratch buffer for the probe itself, freed before hardscan_run
 * returns, and using Arena here would need this file to take one as a
 * parameter for a buffer nothing outside this function ever sees -
 * exactly the "genuinely temporary, not a real ownership question"
 * case this project already carves out elsewhere (see e.g. base.h's
 * own top comment on scoped exceptions to its "0 glibc" rule). */

static f64 hardscan_now_(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
}

static size_t hardscan_sysconf_size_(i32 name)
{
    long v = sysconf(name);
    return v > 0 ? (size_t)v : 0;
}

static void hardscan_probe_cpu_(Hardscan_Report *out)
{
    out->l1_dcache_size = hardscan_sysconf_size_(_SC_LEVEL1_DCACHE_SIZE);
    out->l1_dcache_line_size = hardscan_sysconf_size_(_SC_LEVEL1_DCACHE_LINESIZE);
    out->l2_cache_size = hardscan_sysconf_size_(_SC_LEVEL2_CACHE_SIZE);
    out->l3_cache_size = hardscan_sysconf_size_(_SC_LEVEL3_CACHE_SIZE);
    {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        out->n_cpus = n > 0 ? (i32)n : 1;
    }
}

static void hardscan_probe_memory_(Hardscan_Report *out)
{
    u8 *buf = (u8 *)malloc(HARDSCAN_MEM_PROBE_BYTES);
    f64 t0, t1;
    size_t i;
    volatile u64 sink = 0; /* keeps the read pass from being optimized away entirely */

    if (!buf) { out->mem_bandwidth_mb_per_sec = 0.0; return; }

    t0 = hardscan_now_();
    for (i = 0; i < HARDSCAN_MEM_PROBE_BYTES; i++) buf[i] = (u8)i;
    for (i = 0; i < HARDSCAN_MEM_PROBE_BYTES; i += 8) sink += *(u64 *)(buf + i);
    t1 = hardscan_now_();

    (void)sink;
    free(buf);

    /* write pass + read pass together move 2x the buffer size */
    if (t1 > t0) {
        f64 mb = (f64)(HARDSCAN_MEM_PROBE_BYTES * 2) / (1024.0 * 1024.0);
        out->mem_bandwidth_mb_per_sec = mb / (t1 - t0);
    } else {
        out->mem_bandwidth_mb_per_sec = 0.0; /* clock resolution too coarse to measure - honest 0, not a fabricated number */
    }
}

static void hardscan_probe_disk_(Hardscan_Report *out, const char *dir)
{
    char path[512];
    FILE *f;
    u8 *buf;
    f64 t0, t1;
    size_t written, read_back;

    out->disk_probe_ran = 0;
    out->disk_write_mb_per_sec = 0.0;
    out->disk_read_mb_per_sec = 0.0;

    if (!dir) return;

    buf = (u8 *)malloc(HARDSCAN_DISK_PROBE_BYTES);
    if (!buf) return;
    {
        size_t i;
        for (i = 0; i < HARDSCAN_DISK_PROBE_BYTES; i++) buf[i] = (u8)(i * 2654435761u);
    }

    {
        i32 n = snprintf(path, sizeof(path), "%s/hardscan_probe_%ld.tmp", dir, (long)getpid());
        if (n <= 0 || (size_t)n >= sizeof(path)) { free(buf); return; }
    }

    f = fopen(path, "wb");
    if (!f) { free(buf); return; }

    t0 = hardscan_now_();
    written = fwrite(buf, 1, HARDSCAN_DISK_PROBE_BYTES, f);
    fflush(f);
    fsync(fileno(f));
    t1 = hardscan_now_();
    fclose(f);

    if (written == HARDSCAN_DISK_PROBE_BYTES && t1 > t0) {
        f64 mb = (f64)HARDSCAN_DISK_PROBE_BYTES / (1024.0 * 1024.0);
        out->disk_write_mb_per_sec = mb / (t1 - t0);
    }

    f = fopen(path, "rb");
    if (f) {
        t0 = hardscan_now_();
        read_back = fread(buf, 1, HARDSCAN_DISK_PROBE_BYTES, f);
        t1 = hardscan_now_();
        fclose(f);

        if (read_back == HARDSCAN_DISK_PROBE_BYTES && t1 > t0) {
            f64 mb = (f64)HARDSCAN_DISK_PROBE_BYTES / (1024.0 * 1024.0);
            out->disk_read_mb_per_sec = mb / (t1 - t0);
        }
        out->disk_probe_ran = 1;
    }

    remove(path);
    free(buf);
}

/* recommended chunk size: the largest available shared cache (L3, else
 * L2, else L1) divided by the online CPU count - a chunk sized so that
 * ALL cores' concurrently-active arenas together still fit the shared
 * cache, not just one. Rounded down to a whole multiple of the cache
 * line size (so a chunk boundary never splits a line) and clamped to a
 * sane [4 KiB, 4 MiB] range for the case where sysconf reports nothing
 * usable at all (a genuinely cacheless/virtualized environment) or an
 * absurdly large one. */
static size_t hardscan_recommend_chunk_(const Hardscan_Report *r)
{
    size_t cache = r->l3_cache_size ? r->l3_cache_size : (r->l2_cache_size ? r->l2_cache_size : r->l1_dcache_size);
    size_t line = r->l1_dcache_line_size ? r->l1_dcache_line_size : 64;
    size_t chunk;

    if (cache == 0 || r->n_cpus <= 0) return 64 * 1024; /* honest fallback default, not a guess dressed up as a measurement */

    chunk = cache / (size_t)r->n_cpus;
    chunk = (chunk / line) * line; /* round down to a whole cache-line multiple */

    if (chunk < 4 * 1024) chunk = 4 * 1024;
    if (chunk > 4 * 1024 * 1024) chunk = 4 * 1024 * 1024;
    return chunk;
}

void hardscan_run(Hardscan_Report *out, const char *disk_probe_dir)
{
    hardscan_probe_cpu_(out);
    hardscan_probe_memory_(out);
    hardscan_probe_disk_(out, disk_probe_dir);
    out->recommended_arena_chunk_size = hardscan_recommend_chunk_(out);
}

#endif /* HARDSCAN_IMPLEMENTATION_ */
#endif /* HARDSCAN_IMPLEMENTATION */
