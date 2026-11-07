/* test_math.c - every kernel against a plain scalar reference, across awkward lengths (0, 1, 7, 8, 9, 63, 1000) and unaligned starts.
 * Elementwise results must be BIT-identical to the reference (the library promises determinism, not "close enough"). */
#include "ecs_math.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

static unsigned rng = 12345;
static float rnd(void) { rng = rng * 1664525u + 1013904223u; return (float)((rng >> 8) & 0xFFFF) / 65535.0f * 200.0f - 100.0f; }

static int same(const float *a, const float *b, size_t n) { return memcmp(a, b, n * sizeof(float)) == 0; }

int main(void)
{
	static const size_t lens[] = { 0, 1, 2, 7, 8, 9, 15, 16, 17, 63, 64, 1000 };
	enum { MAXN = 1100 };
	static float A[MAXN + 8], B[MAXN + 8], R[MAXN + 8], D[MAXN + 8];

	for (size_t li = 0; li < sizeof lens / sizeof *lens; li++) {
		size_t n = lens[li];
		for (size_t off = 0; off < 3; off++) {            /* unaligned starts */
			float *a = A + off, *b = B + off, *r = R + off, *d = D + off;
			for (size_t i = 0; i < n; i++) { a[i] = rnd(); b[i] = rnd(); }

			ecs_f32_add(d, a, b, n);   for (size_t i = 0; i < n; i++) r[i] = a[i] + b[i];
			CHECK(same(d, r, n), "add");
			ecs_f32_sub(d, a, b, n);   for (size_t i = 0; i < n; i++) r[i] = a[i] - b[i];
			CHECK(same(d, r, n), "sub");
			ecs_f32_mul(d, a, b, n);   for (size_t i = 0; i < n; i++) r[i] = a[i] * b[i];
			CHECK(same(d, r, n), "mul");
			ecs_f32_scale(d, a, 0.37f, n); for (size_t i = 0; i < n; i++) r[i] = a[i] * 0.37f;
			CHECK(same(d, r, n), "scale");
			ecs_f32_clamp(d, a, -10.0f, 25.0f, n);
			for (size_t i = 0; i < n; i++) r[i] = a[i] < -10.0f ? -10.0f : (a[i] > 25.0f ? 25.0f : a[i]);
			CHECK(same(d, r, n), "clamp");
			ecs_f32_lerp(d, a, b, 0.25f, n);
			for (size_t i = 0; i < n; i++) { float t = (b[i] - a[i]) * 0.25f; r[i] = a[i] + t; }
			CHECK(same(d, r, n), "lerp");

			/* in-place (dst == a) must work */
			memcpy(d, a, n * sizeof(float));
			ecs_f32_add(d, d, b, n);
			for (size_t i = 0; i < n; i++) r[i] = a[i] + b[i];
			CHECK(same(d, r, n), "add in place");

			/* axpy: dst += x*s, mul then add (never fused) */
			memcpy(d, a, n * sizeof(float));
			ecs_f32_axpy(d, b, 1.5f, n);
			for (size_t i = 0; i < n; i++) { float m = b[i] * 1.5f; r[i] = a[i] + m; }
			CHECK(same(d, r, n), "axpy");

			/* approach: never overshoots, converges */
			memcpy(d, a, n * sizeof(float));
			ecs_f32_approach(d, b, 3.0f, n);
			int ok = 1;
			for (size_t i = 0; i < n; i++) {
				float diff = fabsf(b[i] - a[i]);
				float moved = fabsf(d[i] - a[i]);
				float expect = diff < 3.0f ? diff : 3.0f;
				if (fabsf(moved - expect) > 1e-3f) ok = 0;
				if ((d[i] - b[i]) * (a[i] - b[i]) < -1e-3f) ok = 0;      /* crossed the target */
			}
			CHECK(ok, "approach steps toward the target without overshoot");

			/* reductions: fixed-lane order, so compare against the same lane-order reference */
			float l[8] = { 0 }, dl[8] = { 0 };
			size_t m8 = n & ~(size_t)7, i = 0;
			for (; i < m8; i += 8) for (int k = 0; k < 8; k++) { l[k] += a[i + k]; float p = a[i + k] * b[i + k]; dl[k] += p; }
			float rs = ((l[0] + l[4]) + (l[1] + l[5])) + ((l[2] + l[6]) + (l[3] + l[7]));
			float rd = ((dl[0] + dl[4]) + (dl[1] + dl[5])) + ((dl[2] + dl[6]) + (dl[3] + dl[7]));
			for (; i < n; i++) { rs += a[i]; float p = a[i] * b[i]; rd += p; }
			float gs = ecs_f32_sum(a, n), gd = ecs_f32_dot(a, b, n);
			CHECK(memcmp(&gs, &rs, sizeof gs) == 0, "sum is bit-identical to the fixed-lane reference");
			CHECK(memcmp(&gd, &rd, sizeof gd) == 0, "dot is bit-identical to the fixed-lane reference");

			float mn = INFINITY, mx = -INFINITY;
			for (size_t k = 0; k < n; k++) { if (a[k] < mn) mn = a[k]; if (a[k] > mx) mx = a[k]; }
			CHECK(ecs_f32_min_value(a, n) == mn && ecs_f32_max_value(a, n) == mx, "min/max reductions");

			/* masks / compaction */
			static uint32_t mask[MAXN + 8], idx[MAXN + 8];
			size_t cm = ecs_f32_mask_le(mask, a, 0.0f, n), ci = ecs_f32_select_le(idx, a, 0.0f, n), ref = 0;
			for (size_t k = 0; k < n; k++) if (a[k] <= 0.0f) {
				CHECK(mask[k] == 0xFFFFFFFFu, "mask set");
				if (ref < n) CHECK(idx[ref] == (uint32_t)k, "select index in ascending order");
				ref++;
			} else CHECK(mask[k] == 0u, "mask clear");
			CHECK(cm == ref && ci == ref, "mask/select counts agree with the reference");
		}
	}

	/* vec3 SoA */
	{
		enum { N = 37 };
		float px[N], py[N], pz[N], vx[N], vy[N], vz[N], o[N];
		for (int i = 0; i < N; i++) { px[i] = rnd(); py[i] = rnd(); pz[i] = rnd(); vx[i] = rnd(); vy[i] = rnd(); vz[i] = rnd(); }
		float rx[N], ry[N], rz[N];
		memcpy(rx, px, sizeof px); memcpy(ry, py, sizeof py); memcpy(rz, pz, sizeof pz);
		ecs_v3_integrate(px, py, pz, vx, vy, vz, 0.016f, N);
		for (int i = 0; i < N; i++) { float dx = vx[i] * 0.016f, dy = vy[i] * 0.016f, dz = vz[i] * 0.016f; rx[i] += dx; ry[i] += dy; rz[i] += dz; }
		CHECK(same(px, rx, N) && same(py, ry, N) && same(pz, rz, N), "v3 integrate");

		ecs_v3_dist2_to(o, px, py, pz, 1.0f, 2.0f, 3.0f, N);
		int ok = 1;
		for (int i = 0; i < N; i++) {
			float dx = px[i] - 1.0f, dy = py[i] - 2.0f, dz = pz[i] - 3.0f, xx = dx * dx, yy = dy * dy, zz = dz * dz;
			float e = (xx + yy) + zz;
			if (memcmp(&o[i], &e, sizeof e) != 0) ok = 0;
		}
		CHECK(ok, "v3 dist2");

		float nx[N], ny[N], nz[N], len[N];
		memcpy(nx, vx, sizeof vx); memcpy(ny, vy, sizeof vy); memcpy(nz, vz, sizeof vz);
		nx[3] = ny[3] = nz[3] = 0.0f;                     /* zero-length must stay zero, not NaN */
		ecs_v3_normalize(nx, ny, nz, N);
		ecs_v3_length(len, nx, ny, nz, N);
		ok = 1;
		for (int i = 0; i < N; i++) {
			float want = i == 3 ? 0.0f : 1.0f;
			if (fabsf(len[i] - want) > 1e-5f) ok = 0;
		}
		CHECK(ok && nx[3] == 0.0f, "v3 normalize/length (zero stays zero)");

		uint32_t idx[N];
		size_t c = ecs_v3_within_radius(idx, px, py, pz, 0.0f, 0.0f, 0.0f, 120.0f, N), ref = 0;
		for (int i = 0; i < N; i++) {
			float s = px[i] * px[i] + py[i] * py[i] + pz[i] * pz[i];
			if (s <= 120.0f * 120.0f) { CHECK(ref < c && idx[ref] == (uint32_t)i, "within_radius ascending"); ref++; }
		}
		CHECK(c == ref, "within_radius count");
	}

	/* scalar helpers */
	CHECK(ecs_softcap(0, 40, 60, 300, 1100, 1900) == 300.0f, "softcap at 0");
	CHECK(fabsf(ecs_softcap(40, 40, 60, 300, 1100, 1900) - 1100.0f) < 1e-3f, "softcap at soft");
	CHECK(fabsf(ecs_softcap(60, 40, 60, 300, 1100, 1900) - 1900.0f) < 1e-3f, "softcap at hard");
	CHECK(ecs_softcap(99, 40, 60, 300, 1100, 1900) > 1900.0f && ecs_softcap(99, 40, 60, 300, 1100, 1900) < 1900.0f + 39 * 40.0f,
	      "softcap tail still grows but slower than the last segment");

	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("test_math: ok\n");
	return 0;
}
