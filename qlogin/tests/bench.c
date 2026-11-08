/* bench: 10000 GetSession calls against a live qlogind on a private bus. Driven by tests/run_bench (env QT_BUS). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define QDBUS_IMPLEMENTATION
#include "../src/mods/qdbus.h"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
int main(void)
{
	qd_conn c; qd_msg r; qd_buf b = {0};
	enum { N = 10000 };
	static double lat[N];
	if (qd_open(&c, getenv("QT_BUS"))) { fprintf(stderr, "bench: no bus\n"); return 1; }
	qd_put_u32(&b, getuid()); qd_put_u32(&b, getpid()); qd_put_str(&b, "bench"); qd_put_str(&b, "tty"); qd_put_str(&b, "user"); qd_put_str(&b, "");
	qd_put_str(&b, ""); qd_put_u32(&b, 0); qd_put_str(&b, ""); qd_put_str(&b, ""); qd_put_u32(&b, 0); qd_put_str(&b, ""); qd_put_str(&b, "");
	{ qd_arr a = qd_arr_begin(&b, 8); qd_arr_end(&b, a); }
	if (qd_call(&c, &r, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "CreateSession", "uusssssussbssa(sv)", &b, NULL, 0, 3000) != 0) { fprintf(stderr, "bench: CreateSession failed\n"); return 2; }
	r.fds[0] = -1;   /* keep the fifo open: closing it ends the session */
	qd_msg_free(&r);
	double t0 = now();
	for (int i = 0; i < N; i++) {
		double s = now();
		b.n = 0; qd_put_str(&b, "c1");
		if (qd_call(&c, &r, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "GetSession", "s", &b, NULL, 0, 3000) != 0) { fprintf(stderr, "bench: GetSession failed\n"); return 3; }
		qd_msg_free(&r);
		lat[i] = now() - s;
	}
	double tot = now() - t0;
	qsort(lat, N, sizeof *lat, cmp);
	printf("GetSession x%d: total %.1f ms, mean %.1f us, p50 %.1f us, p99 %.1f us, %.0f calls/s\n", N, tot / 1000, tot / N, lat[N / 2], lat[N * 99 / 100], N / (tot / 1e6));
	qd_close(&c); free(b.b);
	return 0;
}
