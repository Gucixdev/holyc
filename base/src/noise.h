#ifndef HOLYC_NOISE_H_
#define HOLYC_NOISE_H_

/* Noise primitives, one file per the single-file convention
 * directive - previously split into src/math/noise/{blue,fbm,simplex,
 * voronoi}.h, merged back here. Each algorithm below still stands alone
 * under its OWN IMPLEMENTATION guard macro (BLUE_/FBM_/SIMPLEX_/
 * VORONOI_IMPLEMENTATION, unchanged from the split files) - a caller/test
 * wanting only one algorithm still only pulls in that one's code, this file
 * just puts them in one place on disk. fbm's implementation calls into
 * simplex's, so a caller defining FBM_IMPLEMENTATION must also define
 * SIMPLEX_IMPLEMENTATION (same requirement the split files had). */

#include "base.h"
#include "arena_alloc.h"
#include "rng.h"

/* ---- blue ----
 * Blue noise via Bridson's Poisson-disk sampling (Robert Bridson, "Fast
 * Poisson Disk Sampling in Arbitrary Dimensions", 2007) - not a port of
 * anything in holyc_download/: FastNoiseLite (the source for simplex/
 * voronoi below) has no blue noise at all, and par_bluenoise.h was reviewed
 * and rejected earlier (requires downloading a 2MB external binary
 * tileset at runtime, not self-contained). This is a from-scratch
 * implementation of a well-known, simple, O(n) algorithm instead.
 *
 * Produces POINTS, not a per-coordinate value field like simplex/voronoi
 * below - blue noise's actual use (scattering, stippling, sample
 * placement) wants a point set with a guaranteed minimum spacing and no
 * clustering/periodicity, not `noise(x, y) -> f32`. A background grid
 * (cell size min_dist/sqrt(2), so any two points sharing a cell are
 * necessarily within min_dist of each other) gives O(1) amortized
 * neighbor lookups instead of an O(n) scan against every existing point
 * per candidate. */

typedef struct {
    f32 x, y;
} Blue_Point;

/* samples points in [0,width) x [0,height) such that no two points are
 * closer than min_dist, with good spatial coverage - the actual
 * blue-noise property (no clustering the way uniform-random sampling
 * produces, no strong periodicity the way a regular grid produces).
 * max_attempts is candidates tried per active point before giving up on
 * it (30 is Bridson's own recommended default - higher packs tighter at
 * more cost, lower leaves visibly larger gaps). Returns the point count;
 * *out_points is set to an arena-owned array of that length. */
size_t blue_noise_sample_2d(Arena *a, u32 seed, f32 width, f32 height, f32 min_dist,
                             i32 max_attempts, Blue_Point **out_points);

/* ---- simplex ----
 * Ported from FastNoiseLite (Jordan Peck + contributors, MIT), the
 * OPENSIMPLEX2 noise type only - not the "S" variant, not Perlin/Value/
 * Cellular, not fractal layering or domain warp (those are generic
 * wrappers any noise primitive could sit under, not specific to this
 * algorithm, and this project has no use for them yet). Coordinate
 * skew/rotation (folded into a separate "TransformNoiseCoordinate" step
 * in the original, gated by frequency/rotation-mode config on a shared
 * state struct) is applied unconditionally here instead, matching
 * OPENSIMPLEX2's own default (FNL_ROTATION_NONE) - the alternate 3D
 * "improve XY/XZ planes" rotations that trade isotropy in one plane for
 * more of it in another were left out as configuration this project
 * doesn't need yet, not as a simplification of the algorithm itself.
 *
 * Output is the SAME normalized range as upstream (empirically-derived
 * magic constants baked into the return statements, not re-derived here)
 * - roughly [-1, 1], verified empirically in tests/test_noise.c rather
 * than proven analytically (neither this port nor the original proves a
 * tight bound; "roughly" is accurate). */

f32 simplex_noise_2d(i32 seed, f32 x, f32 y);
f32 simplex_noise_3d(i32 seed, f32 x, f32 y, f32 z);

/* ---- fbm ----
 * Fractal Brownian Motion - a thin layer summing multiple OCTAVES of
 * simplex noise above at increasing frequency and decreasing amplitude,
 * the standard technique for more natural/detailed noise (terrain
 * heightmaps, cloud/texture generation). Not a new noise primitive of its
 * own - every octave is a real simplex_noise_2d/3d call, this only does
 * the frequency/amplitude bookkeeping and summation around it. Each
 * octave uses a distinct derived seed (seed+i) so octaves are not
 * trivially correlated with each other.
 *
 * lacunarity: frequency multiplier per octave (conventionally ~2.0 -
 * each octave samples at roughly double the previous octave's
 * frequency, adding finer detail). gain (persistence): amplitude
 * multiplier per octave (conventionally ~0.5 - each octave contributes
 * roughly half the previous octave's weight, so finer detail matters
 * less than coarse shape). Output is NOT renormalized to [-1,1] - with
 * gain<1 the total amplitude is a convergent geometric series, staying
 * a modest, predictable multiple of simplex's own ~[-1,1] range for the
 * conventional gain=0.5 case (converges toward 2x) - not proven
 * analytically, same "empirically roughly bounded" honesty simplex's
 * own comment above already uses for its own output range. */

f32 fbm_2d(i32 seed, f32 x, f32 y, i32 octaves, f32 lacunarity, f32 gain);
f32 fbm_3d(i32 seed, f32 x, f32 y, f32 z, i32 octaves, f32 lacunarity, f32 gain);

/* ---- voronoi ----
 * Ported from FastNoiseLite (Jordan Peck + contributors, MIT), the
 * cellular/Voronoi noise type - Euclidean distance only (their other
 * distance functions - EuclideanSq, Manhattan, Hybrid - and their
 * CellValue/Distance2Add/Distance2Mul/Distance2Div return modes were left
 * out as configuration this project doesn't need yet, not simplifications
 * of the core algorithm).
 *
 * Reshaped into a single query (`voronoi_query_2d`/`_3d`) that computes
 * everything a caller could want (nearest feature distance, second-
 * nearest distance, and the nearest feature's hash) in one pass over the
 * 3x3 (2D) / 3x3x3 (3D) neighboring cells, rather than upstream's
 * "return type" switch that would force a caller wanting more than one
 * property to redo that search per property. `voronoi_edge_2d`/`_3d`
 * (second-nearest minus nearest - the classic cell-boundary-highlighting
 * quantity) are thin convenience wrappers over the query.
 *
 * `distance0`/`distance1` are plain sqrt'd Euclidean distances (no -1
 * offset the way upstream centers its DISTANCE return mode around 0 for
 * direct use as fractal-layer noise) - this project isn't feeding these
 * into a fractal accumulator, so raw non-negative distances are the more
 * generally useful shape. `jitter` is exposed directly (upstream's
 * `cellular_jitter_mod`, 1.0 == full jitter matching the library's own
 * baked-in per-dimension constant, 0.0 == perfectly regular grid, useful
 * for testing determinism). */

typedef struct {
    f32 distance0; /* nearest feature point, Euclidean, >= 0 */
    f32 distance1; /* second-nearest feature point, Euclidean, >= distance0 */
    i32 closest_hash; /* hash identifying the nearest feature/cell - stable per cell, differs across cells */
} Voronoi_Query;

void voronoi_query_2d(i32 seed, f32 x, f32 y, f32 jitter, Voronoi_Query *out);
void voronoi_query_3d(i32 seed, f32 x, f32 y, f32 z, f32 jitter, Voronoi_Query *out);

/* [-1, 1]-ish, derived from closest_hash - a per-cell "identity" value,
 * useful for e.g. coloring each Voronoi cell differently. */
f32 voronoi_cell_value(const Voronoi_Query *q);

/* second-nearest minus nearest distance - near 0 close to a cell edge,
 * larger toward a cell's interior. */
f32 voronoi_edge_2d(i32 seed, f32 x, f32 y, f32 jitter);
f32 voronoi_edge_3d(i32 seed, f32 x, f32 y, f32 z, f32 jitter);

#endif /* HOLYC_NOISE_H_ */

#ifdef BLUE_IMPLEMENTATION
#ifndef BLUE_IMPLEMENTATION_
#define BLUE_IMPLEMENTATION_

#define BLUE_PI_ 3.14159265358979323846

/* Range-reduced Taylor-series sin/cos, replacing math.h's - own local
 * copy (not shared with any other file's), matching base.h's own
 * vector_sqrt_/quat_sin_/quat_cos_ pattern (see that file's comment
 * for the accuracy/truncation rationale, and this project's "0 glibc"
 * push, no-glibc rule). */
static f64 blue_mod_2pi_(f64 x)
{
    const f64 two_pi = 6.283185307179586476925286766559;
    const f64 pi = 3.14159265358979323846;
    f64 n = (f64)(i64)(x / two_pi);
    f64 r = x - n * two_pi;
    if (r > pi) r -= two_pi;
    if (r < -pi) r += two_pi;
    return r;
}

static f64 blue_sin_(f64 x)
{
    f64 r = blue_mod_2pi_(x);
    f64 x2 = r * r;
    f64 term = r;
    f64 sum = r;
    i32 n;
    for (n = 1; n <= 6; n++) {
        term *= -x2 / ((f64)(2 * n) * (f64)(2 * n + 1));
        sum += term;
    }
    return sum;
}

static f64 blue_cos_(f64 x)
{
    f64 r = blue_mod_2pi_(x);
    f64 x2 = r * r;
    f64 term = 1.0;
    f64 sum = 1.0;
    i32 n;
    for (n = 1; n <= 6; n++) {
        term *= -x2 / ((f64)(2 * n - 1) * (f64)(2 * n));
        sum += term;
    }
    return sum;
}

static f32 blue_dist_sq_(f32 ax, f32 ay, f32 bx, f32 by)
{
    f32 dx = ax - bx;
    f32 dy = ay - by;
    return dx * dx + dy * dy;
}

size_t blue_noise_sample_2d(Arena *a, u32 seed, f32 width, f32 height, f32 min_dist,
                             i32 max_attempts, Blue_Point **out_points)
{
    Rng rng;
    f32 cell_size;
    i32 grid_w, grid_h;
    i32 *grid; /* [grid_w * grid_h], index into points[], -1 for empty */
    Blue_Point *points;
    size_t points_count, points_capacity;
    i64 *active; /* indices into points[] still worth trying to grow around */
    size_t active_count, active_capacity;
    i32 gx, gy;

    pcg32_seed(&rng, (u64)seed, 1);

    cell_size = min_dist / 1.41421356f; /* min_dist / sqrt(2) - see file header */
    grid_w = (i32)(width / cell_size) + 1;
    grid_h = (i32)(height / cell_size) + 1;

    grid = (i32 *)arena_alloc(a, (size_t)grid_w * (size_t)grid_h * sizeof(i32));
    for (gy = 0; gy < grid_h; gy++)
        for (gx = 0; gx < grid_w; gx++)
            grid[gy * grid_w + gx] = -1;

    points_capacity = 64;
    points = (Blue_Point *)arena_alloc(a, points_capacity * sizeof(Blue_Point));
    points_count = 0;

    active_capacity = 64;
    active = (i64 *)arena_alloc(a, active_capacity * sizeof(i64));
    active_count = 0;

    {
        Blue_Point p;
        p.x = pcg32_f32(&rng) * width;
        p.y = pcg32_f32(&rng) * height;
        points[points_count] = p;
        active[active_count++] = (i64)points_count;
        grid[((i32)(p.y / cell_size)) * grid_w + (i32)(p.x / cell_size)] = (i32)points_count;
        points_count++;
    }

    while (active_count > 0) {
        size_t pick = (size_t)pcg32_range(&rng, 0, (u32)active_count);
        Blue_Point base = points[active[pick]];
        i32 found = 0;
        i32 attempt;

        for (attempt = 0; attempt < max_attempts; attempt++) {
            f32 angle = pcg32_f32(&rng) * 2.0f * (f32)BLUE_PI_;
            f32 radius = min_dist * (1.0f + pcg32_f32(&rng)); /* [min_dist, 2*min_dist) */
            Blue_Point cand;
            i32 cgx, cgy, nx, ny, ok;

            cand.x = base.x + radius * (f32)blue_cos_((f64)angle);
            cand.y = base.y + radius * (f32)blue_sin_((f64)angle);

            if (cand.x < 0.0f || cand.x >= width || cand.y < 0.0f || cand.y >= height) continue;

            cgx = (i32)(cand.x / cell_size);
            cgy = (i32)(cand.y / cell_size);

            ok = 1;
            for (ny = cgy - 2; ny <= cgy + 2 && ok; ny++) {
                if (ny < 0 || ny >= grid_h) continue;
                for (nx = cgx - 2; nx <= cgx + 2; nx++) {
                    i32 neighbor_idx;
                    if (nx < 0 || nx >= grid_w) continue;
                    neighbor_idx = grid[ny * grid_w + nx];
                    if (neighbor_idx < 0) continue;
                    if (blue_dist_sq_(cand.x, cand.y, points[neighbor_idx].x, points[neighbor_idx].y)
                        < min_dist * min_dist) {
                        ok = 0;
                        break;
                    }
                }
            }

            if (ok) {
                if (points_count >= points_capacity) {
                    size_t new_cap = points_capacity * 2;
                    points = (Blue_Point *)arena_realloc(a, points, points_capacity * sizeof(Blue_Point), new_cap * sizeof(Blue_Point));
                    points_capacity = new_cap;
                }
                if (active_count >= active_capacity) {
                    size_t new_cap = active_capacity * 2;
                    active = (i64 *)arena_realloc(a, active, active_capacity * sizeof(i64), new_cap * sizeof(i64));
                    active_capacity = new_cap;
                }
                points[points_count] = cand;
                grid[cgy * grid_w + cgx] = (i32)points_count;
                active[active_count++] = (i64)points_count;
                points_count++;
                found = 1;
                break;
            }
        }

        if (!found) {
            active[pick] = active[active_count - 1];
            active_count--;
        }
    }

    *out_points = points;
    return points_count;
}

#endif /* BLUE_IMPLEMENTATION_ */
#endif /* BLUE_IMPLEMENTATION */

#ifdef SIMPLEX_IMPLEMENTATION
#ifndef SIMPLEX_IMPLEMENTATION_
#define SIMPLEX_IMPLEMENTATION_

/* hash-space lattice coordinates deliberately overflow (wrap mod 2^32) when
 * multiplied by these primes - that's the actual hashing mechanism, not a
 * bug, so the whole pipeline below (primes, xp/yp/zp, hash) is u32, not
 * i32: unsigned overflow is well-defined wraparound in C, giving the exact
 * same bit pattern a wrapping-signed multiply would produce on real
 * hardware, without invoking UB. */
static const u32 simplex_prime_x_ = 501125321u;
static const u32 simplex_prime_y_ = 1136930381u;
static const u32 simplex_prime_z_ = 1720413743u;

/* 128 (x,y) pairs, each repeated so a masked hash (mod 128, doubled for
 * the pair stride) lands on a roughly uniform spread of unit-circle
 * directions - upstream's own table, copied verbatim. */
static const f32 simplex_gradients_2d_[] = {
    0.130526192220052f, 0.99144486137381f, 0.38268343236509f, 0.923879532511287f, 0.608761429008721f, 0.793353340291235f, 0.793353340291235f, 0.608761429008721f,
    0.923879532511287f, 0.38268343236509f, 0.99144486137381f, 0.130526192220051f, 0.99144486137381f, -0.130526192220051f, 0.923879532511287f, -0.38268343236509f,
    0.793353340291235f, -0.60876142900872f, 0.608761429008721f, -0.793353340291235f, 0.38268343236509f, -0.923879532511287f, 0.130526192220052f, -0.99144486137381f,
    -0.130526192220052f, -0.99144486137381f, -0.38268343236509f, -0.923879532511287f, -0.608761429008721f, -0.793353340291235f, -0.793353340291235f, -0.608761429008721f,
    -0.923879532511287f, -0.38268343236509f, -0.99144486137381f, -0.130526192220052f, -0.99144486137381f, 0.130526192220051f, -0.923879532511287f, 0.38268343236509f,
    -0.793353340291235f, 0.608761429008721f, -0.608761429008721f, 0.793353340291235f, -0.38268343236509f, 0.923879532511287f, -0.130526192220052f, 0.99144486137381f,
    0.130526192220052f, 0.99144486137381f, 0.38268343236509f, 0.923879532511287f, 0.608761429008721f, 0.793353340291235f, 0.793353340291235f, 0.608761429008721f,
    0.923879532511287f, 0.38268343236509f, 0.99144486137381f, 0.130526192220051f, 0.99144486137381f, -0.130526192220051f, 0.923879532511287f, -0.38268343236509f,
    0.793353340291235f, -0.60876142900872f, 0.608761429008721f, -0.793353340291235f, 0.38268343236509f, -0.923879532511287f, 0.130526192220052f, -0.99144486137381f,
    -0.130526192220052f, -0.99144486137381f, -0.38268343236509f, -0.923879532511287f, -0.608761429008721f, -0.793353340291235f, -0.793353340291235f, -0.608761429008721f,
    -0.923879532511287f, -0.38268343236509f, -0.99144486137381f, -0.130526192220052f, -0.99144486137381f, 0.130526192220051f, -0.923879532511287f, 0.38268343236509f,
    -0.793353340291235f, 0.608761429008721f, -0.608761429008721f, 0.793353340291235f, -0.38268343236509f, 0.923879532511287f, -0.130526192220052f, 0.99144486137381f,
    0.130526192220052f, 0.99144486137381f, 0.38268343236509f, 0.923879532511287f, 0.608761429008721f, 0.793353340291235f, 0.793353340291235f, 0.608761429008721f,
    0.923879532511287f, 0.38268343236509f, 0.99144486137381f, 0.130526192220051f, 0.99144486137381f, -0.130526192220051f, 0.923879532511287f, -0.38268343236509f,
    0.793353340291235f, -0.60876142900872f, 0.608761429008721f, -0.793353340291235f, 0.38268343236509f, -0.923879532511287f, 0.130526192220052f, -0.99144486137381f,
    -0.130526192220052f, -0.99144486137381f, -0.38268343236509f, -0.923879532511287f, -0.608761429008721f, -0.793353340291235f, -0.793353340291235f, -0.608761429008721f,
    -0.923879532511287f, -0.38268343236509f, -0.99144486137381f, -0.130526192220052f, -0.99144486137381f, 0.130526192220051f, -0.923879532511287f, 0.38268343236509f,
    -0.793353340291235f, 0.608761429008721f, -0.608761429008721f, 0.793353340291235f, -0.38268343236509f, 0.923879532511287f, -0.130526192220052f, 0.99144486137381f,
    0.130526192220052f, 0.99144486137381f, 0.38268343236509f, 0.923879532511287f, 0.608761429008721f, 0.793353340291235f, 0.793353340291235f, 0.608761429008721f,
    0.923879532511287f, 0.38268343236509f, 0.99144486137381f, 0.130526192220051f, 0.99144486137381f, -0.130526192220051f, 0.923879532511287f, -0.38268343236509f,
    0.793353340291235f, -0.60876142900872f, 0.608761429008721f, -0.793353340291235f, 0.38268343236509f, -0.923879532511287f, 0.130526192220052f, -0.99144486137381f,
    -0.130526192220052f, -0.99144486137381f, -0.38268343236509f, -0.923879532511287f, -0.608761429008721f, -0.793353340291235f, -0.793353340291235f, -0.608761429008721f,
    -0.923879532511287f, -0.38268343236509f, -0.99144486137381f, -0.130526192220052f, -0.99144486137381f, 0.130526192220051f, -0.923879532511287f, 0.38268343236509f,
    -0.793353340291235f, 0.608761429008721f, -0.608761429008721f, 0.793353340291235f, -0.38268343236509f, 0.923879532511287f, -0.130526192220052f, 0.99144486137381f,
    0.130526192220052f, 0.99144486137381f, 0.38268343236509f, 0.923879532511287f, 0.608761429008721f, 0.793353340291235f, 0.793353340291235f, 0.608761429008721f,
    0.923879532511287f, 0.38268343236509f, 0.99144486137381f, 0.130526192220051f, 0.99144486137381f, -0.130526192220051f, 0.923879532511287f, -0.38268343236509f,
    0.793353340291235f, -0.60876142900872f, 0.608761429008721f, -0.793353340291235f, 0.38268343236509f, -0.923879532511287f, 0.130526192220052f, -0.99144486137381f,
    -0.130526192220052f, -0.99144486137381f, -0.38268343236509f, -0.923879532511287f, -0.608761429008721f, -0.793353340291235f, -0.793353340291235f, -0.608761429008721f,
    -0.923879532511287f, -0.38268343236509f, -0.99144486137381f, -0.130526192220052f, -0.99144486137381f, 0.130526192220051f, -0.923879532511287f, 0.38268343236509f,
    -0.793353340291235f, 0.608761429008721f, -0.608761429008721f, 0.793353340291235f, -0.38268343236509f, 0.923879532511287f, -0.130526192220052f, 0.99144486137381f,
    0.38268343236509f, 0.923879532511287f, 0.923879532511287f, 0.38268343236509f, 0.923879532511287f, -0.38268343236509f, 0.38268343236509f, -0.923879532511287f,
    -0.38268343236509f, -0.923879532511287f, -0.923879532511287f, -0.38268343236509f, -0.923879532511287f, 0.38268343236509f, -0.38268343236509f, 0.923879532511287f
};

/* 64 (x,y,z,0) quadruplets - the trailing 0 keeps every entry a fixed
 * 4-float stride matching the 2-bit-shifted hash mask below, unrelated to
 * any actual w/4th dimension. */
static const f32 simplex_gradients_3d_[] = {
    0, 1, 1, 0,  0,-1, 1, 0,  0, 1,-1, 0,  0,-1,-1, 0,
    1, 0, 1, 0, -1, 0, 1, 0,  1, 0,-1, 0, -1, 0,-1, 0,
    1, 1, 0, 0, -1, 1, 0, 0,  1,-1, 0, 0, -1,-1, 0, 0,
    0, 1, 1, 0,  0,-1, 1, 0,  0, 1,-1, 0,  0,-1,-1, 0,
    1, 0, 1, 0, -1, 0, 1, 0,  1, 0,-1, 0, -1, 0,-1, 0,
    1, 1, 0, 0, -1, 1, 0, 0,  1,-1, 0, 0, -1,-1, 0, 0,
    0, 1, 1, 0,  0,-1, 1, 0,  0, 1,-1, 0,  0,-1,-1, 0,
    1, 0, 1, 0, -1, 0, 1, 0,  1, 0,-1, 0, -1, 0,-1, 0,
    1, 1, 0, 0, -1, 1, 0, 0,  1,-1, 0, 0, -1,-1, 0, 0,
    0, 1, 1, 0,  0,-1, 1, 0,  0, 1,-1, 0,  0,-1,-1, 0,
    1, 0, 1, 0, -1, 0, 1, 0,  1, 0,-1, 0, -1, 0,-1, 0,
    1, 1, 0, 0, -1, 1, 0, 0,  1,-1, 0, 0, -1,-1, 0, 0,
    0, 1, 1, 0,  0,-1, 1, 0,  0, 1,-1, 0,  0,-1,-1, 0,
    1, 0, 1, 0, -1, 0, 1, 0,  1, 0,-1, 0, -1, 0,-1, 0,
    1, 1, 0, 0, -1, 1, 0, 0,  1,-1, 0, 0, -1,-1, 0, 0,
    1, 1, 0, 0,  0,-1, 1, 0, -1, 1, 0, 0,  0,-1,-1, 0
};

static i32 simplex_floor_(f32 f)
{
    i32 fi = (i32)f;
    return fi - (i32)(f < (f32)fi);
}

static i32 simplex_round_(f32 f)
{
    f32 bias = 0.5f - (f32)(f < 0.0f);
    return (i32)(f + bias);
}

static u32 simplex_hash2d_(i32 seed, u32 xp, u32 yp)
{
    u32 hash = (u32)seed ^ xp ^ yp;
    hash *= 0x27d4eb2du;
    return hash;
}

static u32 simplex_hash3d_(i32 seed, u32 xp, u32 yp, u32 zp)
{
    u32 hash = (u32)seed ^ xp ^ yp ^ zp;
    hash *= 0x27d4eb2du;
    return hash;
}

static f32 simplex_grad2d_(i32 seed, u32 xp, u32 yp, f32 xd, f32 yd)
{
    u32 hash = simplex_hash2d_(seed, xp, yp);
    hash ^= hash >> 15;
    hash &= 127u << 1;
    return xd * simplex_gradients_2d_[hash] + yd * simplex_gradients_2d_[hash | 1u];
}

static f32 simplex_grad3d_(i32 seed, u32 xp, u32 yp, u32 zp, f32 xd, f32 yd, f32 zd)
{
    u32 hash = simplex_hash3d_(seed, xp, yp, zp);
    hash ^= hash >> 15;
    hash &= 63u << 2;
    return xd * simplex_gradients_3d_[hash] + yd * simplex_gradients_3d_[hash | 1u] + zd * simplex_gradients_3d_[hash | 2u];
}

f32 simplex_noise_2d(i32 seed, f32 x, f32 y)
{
    const f32 g2 = (3.0f - 1.7320508075688772935f) / 6.0f;
    const f32 f2 = 0.5f * (1.7320508075688772935f - 1.0f);
    f32 skew_t = (x + y) * f2;
    i32 i0, j0;
    u32 i, j;
    f32 xi, yi, t, x0, y0, a, c, n0, n1, n2, a_falloff, c_falloff;

    x += skew_t;
    y += skew_t;

    i0 = simplex_floor_(x);
    j0 = simplex_floor_(y);
    xi = x - (f32)i0;
    yi = y - (f32)j0;

    t = (xi + yi) * g2;
    x0 = xi - t;
    y0 = yi - t;

    i = (u32)i0 * simplex_prime_x_;
    j = (u32)j0 * simplex_prime_y_;

    a = 0.5f - x0 * x0 - y0 * y0;
    a_falloff = (f32)(a > 0.0f);
    n0 = a_falloff * (a * a) * (a * a) * simplex_grad2d_(seed, i, j, x0, y0);

    c = 2.0f * (1.0f - 2.0f * g2) * (1.0f / g2 - 2.0f) * t + (-2.0f * (1.0f - 2.0f * g2) * (1.0f - 2.0f * g2) + a);
    {
        f32 x2 = x0 + (2.0f * g2 - 1.0f);
        f32 y2 = y0 + (2.0f * g2 - 1.0f);
        c_falloff = (f32)(c > 0.0f);
        n2 = c_falloff * (c * c) * (c * c) * simplex_grad2d_(seed, i + simplex_prime_x_, j + simplex_prime_y_, x2, y2);
    }

    /* which second vertex the traversal visits (upper-left vs lower-right
     * triangle of the skewed cell) is a genuine algorithmic choice, not a
     * value clamp - left as a real branch rather than computing (and
     * discarding) both. */
    if (y0 > x0) {
        f32 x1 = x0 + g2;
        f32 y1 = y0 + (g2 - 1.0f);
        f32 b = 0.5f - x1 * x1 - y1 * y1;
        f32 b_falloff = (f32)(b > 0.0f);
        n1 = b_falloff * (b * b) * (b * b) * simplex_grad2d_(seed, i, j + simplex_prime_y_, x1, y1);
    } else {
        f32 x1 = x0 + (g2 - 1.0f);
        f32 y1 = y0 + g2;
        f32 b = 0.5f - x1 * x1 - y1 * y1;
        f32 b_falloff = (f32)(b > 0.0f);
        n1 = b_falloff * (b * b) * (b * b) * simplex_grad2d_(seed, i + simplex_prime_x_, j, x1, y1);
    }

    return (n0 + n1 + n2) * 99.83685446303647f;
}

/* two offset rotated cube grids: a single simplex-like traversal of a
 * cubic (not tetrahedral) lattice would leave gaps, so the loop below
 * runs twice (l==0, l==1) with the second pass sign-flipped and the seed
 * complemented (~seed), sampling a second grid offset to fill them -
 * inherent to OpenSimplex2's 3D construction, not a bug-workaround. */
f32 simplex_noise_3d(i32 seed, f32 x, f32 y, f32 z)
{
    const f32 r3 = 2.0f / 3.0f;
    f32 rot = (x + y + z) * r3;
    i32 i0, j0, k0, x_nsign, y_nsign, z_nsign, l;
    u32 i, j, k;
    f32 x0, y0, z0, ax0, ay0, az0, value, a;

    x = rot - x;
    y = rot - y;
    z = rot - z;

    i0 = simplex_round_(x);
    j0 = simplex_round_(y);
    k0 = simplex_round_(z);
    x0 = x - (f32)i0;
    y0 = y - (f32)j0;
    z0 = z - (f32)k0;

    x_nsign = (i32)(-1.0f - x0) | 1;
    y_nsign = (i32)(-1.0f - y0) | 1;
    z_nsign = (i32)(-1.0f - z0) | 1;

    ax0 = (f32)x_nsign * -x0;
    ay0 = (f32)y_nsign * -y0;
    az0 = (f32)z_nsign * -z0;

    i = (u32)i0 * simplex_prime_x_;
    j = (u32)j0 * simplex_prime_y_;
    k = (u32)k0 * simplex_prime_z_;

    value = 0.0f;
    a = (0.6f - x0 * x0) - (y0 * y0 + z0 * z0);

    for (l = 0; ; l++) {
        f32 b, a_falloff, b_falloff;
        u32 i1, j1, k1;
        f32 x1, y1, z1;

        a_falloff = (f32)(a > 0.0f);
        value += a_falloff * (a * a) * (a * a) * simplex_grad3d_(seed, i, j, k, x0, y0, z0);

        b = a + 1.0f;
        i1 = i; j1 = j; k1 = k;
        x1 = x0; y1 = y0; z1 = z0;

        /* which axis contributes the second lattice vertex is a genuine
         * per-axis-magnitude comparison (the algorithm's actual traversal
         * choice), not a value clamp - kept as a real 3-way branch. */
        if (ax0 >= ay0 && ax0 >= az0) {
            x1 += (f32)x_nsign;
            b -= (f32)x_nsign * 2.0f * x1;
            i1 -= (u32)x_nsign * simplex_prime_x_;
        } else if (ay0 > ax0 && ay0 >= az0) {
            y1 += (f32)y_nsign;
            b -= (f32)y_nsign * 2.0f * y1;
            j1 -= (u32)y_nsign * simplex_prime_y_;
        } else {
            z1 += (f32)z_nsign;
            b -= (f32)z_nsign * 2.0f * z1;
            k1 -= (u32)z_nsign * simplex_prime_z_;
        }

        b_falloff = (f32)(b > 0.0f);
        value += b_falloff * (b * b) * (b * b) * simplex_grad3d_(seed, i1, j1, k1, x1, y1, z1);

        if (l == 1) break;

        ax0 = 0.5f - ax0;
        ay0 = 0.5f - ay0;
        az0 = 0.5f - az0;

        x0 = (f32)x_nsign * ax0;
        y0 = (f32)y_nsign * ay0;
        z0 = (f32)z_nsign * az0;

        a += (0.75f - ax0) - (ay0 + az0);

        i += (u32)(x_nsign >> 1) & simplex_prime_x_;
        j += (u32)(y_nsign >> 1) & simplex_prime_y_;
        k += (u32)(z_nsign >> 1) & simplex_prime_z_;

        x_nsign = -x_nsign;
        y_nsign = -y_nsign;
        z_nsign = -z_nsign;

        seed = ~seed;
    }

    return value * 32.69428253173828125f;
}

#endif /* SIMPLEX_IMPLEMENTATION_ */
#endif /* SIMPLEX_IMPLEMENTATION */

#ifdef FBM_IMPLEMENTATION
#ifndef FBM_IMPLEMENTATION_
#define FBM_IMPLEMENTATION_

f32 fbm_2d(i32 seed, f32 x, f32 y, i32 octaves, f32 lacunarity, f32 gain)
{
    f32 sum = 0.0f;
    f32 amplitude = 1.0f;
    f32 frequency = 1.0f;
    i32 i;

    for (i = 0; i < octaves; i++) {
        sum += simplex_noise_2d(seed + i, x * frequency, y * frequency) * amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }

    return sum;
}

f32 fbm_3d(i32 seed, f32 x, f32 y, f32 z, i32 octaves, f32 lacunarity, f32 gain)
{
    f32 sum = 0.0f;
    f32 amplitude = 1.0f;
    f32 frequency = 1.0f;
    i32 i;

    for (i = 0; i < octaves; i++) {
        sum += simplex_noise_3d(seed + i, x * frequency, y * frequency, z * frequency) * amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }

    return sum;
}

#endif /* FBM_IMPLEMENTATION_ */
#endif /* FBM_IMPLEMENTATION */

#ifdef VORONOI_IMPLEMENTATION
#ifndef VORONOI_IMPLEMENTATION_
#define VORONOI_IMPLEMENTATION_

/* Newton-Raphson sqrt, replacing math.h's - own local copy, matching
 * base.h's own vector_sqrt_ pattern (0 glibc push). FLT_MAX is a
 * compile-time constant, not a real function - the standard's own
 * defined value (IEEE-754 single precision), not a shortcut version
 * of it, so hand-defining it here isn't a loss of precision. */
#ifndef VORONOI_FLT_MAX_
#define VORONOI_FLT_MAX_ 3.402823466e+38f
#endif

static f64 voronoi_sqrt_(f64 x)
{
    f64 guess;
    i32 i;
    if (x <= 0.0) return 0.0;
    guess = (x > 1.0) ? x : 1.0;
    for (i = 0; i < 40; i++) {
        f64 next = 0.5 * (guess + x / guess);
        if (next == guess) break;
        guess = next;
    }
    return guess;
}

static const u32 voronoi_prime_x_ = 501125321u;
static const u32 voronoi_prime_y_ = 1136930381u;
static const u32 voronoi_prime_z_ = 1720413743u;

/* 256 (x,y) jitter-offset directions, upstream's own table, copied
 * verbatim - distinct from simplex's gradient table above (that one's a
 * hand-picked spread of directions for gradient noise; this one's a
 * denser near-uniform spread meant as a per-cell random displacement). */
static const f32 voronoi_rand_vecs_2d_[] = {
    -0.2700222198f, -0.9628540911f, 0.3863092627f, -0.9223693152f, 0.04444859006f, -0.999011673f, -0.5992523158f, -0.8005602176f, -0.7819280288f, 0.6233687174f, 0.9464672271f, 0.3227999196f, -0.6514146797f, -0.7587218957f, 0.9378472289f, 0.347048376f,
    -0.8497875957f, -0.5271252623f, -0.879042592f, 0.4767432447f, -0.892300288f, -0.4514423508f, -0.379844434f, -0.9250503802f, -0.9951650832f, 0.0982163789f, 0.7724397808f, -0.6350880136f, 0.7573283322f, -0.6530343002f, -0.9928004525f, -0.119780055f,
    -0.0532665713f, 0.9985803285f, 0.9754253726f, -0.2203300762f, -0.7665018163f, 0.6422421394f, 0.991636706f, 0.1290606184f, -0.994696838f, 0.1028503788f, -0.5379205513f, -0.84299554f, 0.5022815471f, -0.8647041387f, 0.4559821461f, -0.8899889226f,
    -0.8659131224f, -0.5001944266f, 0.0879458407f, -0.9961252577f, -0.5051684983f, 0.8630207346f, 0.7753185226f, -0.6315704146f, -0.6921944612f, 0.7217110418f, -0.5191659449f, -0.8546734591f, 0.8978622882f, -0.4402764035f, -0.1706774107f, 0.9853269617f,
    -0.9353430106f, -0.3537420705f, -0.9992404798f, 0.03896746794f, -0.2882064021f, -0.9575683108f, -0.9663811329f, 0.2571137995f, -0.8759714238f, -0.4823630009f, -0.8303123018f, -0.5572983775f, 0.05110133755f, -0.9986934731f, -0.8558373281f, -0.5172450752f,
    0.09887025282f, 0.9951003332f, 0.9189016087f, 0.3944867976f, -0.2439375892f, -0.9697909324f, -0.8121409387f, -0.5834613061f, -0.9910431363f, 0.1335421355f, 0.8492423985f, -0.5280031709f, -0.9717838994f, -0.2358729591f, 0.9949457207f, 0.1004142068f,
    0.6241065508f, -0.7813392434f, 0.662910307f, 0.7486988212f, -0.7197418176f, 0.6942418282f, -0.8143370775f, -0.5803922158f, 0.104521054f, -0.9945226741f, -0.1065926113f, -0.9943027784f, 0.445799684f, -0.8951327509f, 0.105547406f, 0.9944142724f,
    -0.992790267f, 0.1198644477f, -0.8334366408f, 0.552615025f, 0.9115561563f, -0.4111755999f, 0.8285544909f, -0.5599084351f, 0.7217097654f, -0.6921957921f, 0.4940492677f, -0.8694339084f, -0.3652321272f, -0.9309164803f, -0.9696606758f, 0.2444548501f,
    0.08925509731f, -0.996008799f, 0.5354071276f, -0.8445941083f, -0.1053576186f, 0.9944343981f, -0.9890284586f, 0.1477251101f, 0.004856104961f, 0.9999882091f, 0.9885598478f, 0.1508291331f, 0.9286129562f, -0.3710498316f, -0.5832393863f, -0.8123003252f,
    0.3015207509f, 0.9534596146f, -0.9575110528f, 0.2883965738f, 0.9715802154f, -0.2367105511f, 0.229981792f, 0.9731949318f, 0.955763816f, -0.2941352207f, 0.740956116f, 0.6715534485f, -0.9971513787f, -0.07542630764f, 0.6905710663f, -0.7232645452f,
    -0.290713703f, -0.9568100872f, 0.5912777791f, -0.8064679708f, -0.9454592212f, -0.325740481f, 0.6664455681f, 0.74555369f, 0.6236134912f, 0.7817328275f, 0.9126993851f, -0.4086316587f, -0.8191762011f, 0.5735419353f, -0.8812745759f, -0.4726046147f,
    0.9953313627f, 0.09651672651f, 0.9855650846f, -0.1692969699f, -0.8495980887f, 0.5274306472f, 0.6174853946f, -0.7865823463f, 0.8508156371f, 0.52546432f, 0.9985032451f, -0.05469249926f, 0.1971371563f, -0.9803759185f, 0.6607855748f, -0.7505747292f,
    -0.03097494063f, 0.9995201614f, -0.6731660801f, 0.739491331f, -0.7195018362f, -0.6944905383f, 0.9727511689f, 0.2318515979f, 0.9997059088f, -0.0242506907f, 0.4421787429f, -0.8969269532f, 0.9981350961f, -0.061043673f, -0.9173660799f, -0.3980445648f,
    -0.8150056635f, -0.5794529907f, -0.8789331304f, 0.4769450202f, 0.0158605829f, 0.999874213f, -0.8095464474f, 0.5870558317f, -0.9165898907f, -0.3998286786f, -0.8023542565f, 0.5968480938f, -0.5176737917f, 0.8555780767f, -0.8154407307f, -0.5788405779f,
    0.4022010347f, -0.9155513791f, -0.9052556868f, -0.4248672045f, 0.7317445619f, 0.6815789728f, -0.5647632201f, -0.8252529947f, -0.8403276335f, -0.5420788397f, -0.9314281527f, 0.363925262f, 0.5238198472f, 0.8518290719f, 0.7432803869f, -0.6689800195f,
    -0.985371561f, -0.1704197369f, 0.4601468731f, 0.88784281f, 0.825855404f, 0.5638819483f, 0.6182366099f, 0.7859920446f, 0.8331502863f, -0.553046653f, 0.1500307506f, 0.9886813308f, -0.662330369f, -0.7492119075f, -0.668598664f, 0.743623444f,
    0.7025606278f, 0.7116238924f, -0.5419389763f, -0.8404178401f, -0.3388616456f, 0.9408362159f, 0.8331530315f, 0.5530425174f, -0.2989720662f, -0.9542618632f, 0.2638522993f, 0.9645630949f, 0.124108739f, -0.9922686234f, -0.7282649308f, -0.6852956957f,
    0.6962500149f, 0.7177993569f, -0.9183535368f, 0.3957610156f, -0.6326102274f, -0.7744703352f, -0.9331891859f, -0.359385508f, -0.1153779357f, -0.9933216659f, 0.9514974788f, -0.3076565421f, -0.08987977445f, -0.9959526224f, 0.6678496916f, 0.7442961705f,
    0.7952400393f, -0.6062947138f, -0.6462007402f, -0.7631674805f, -0.2733598753f, 0.9619118351f, 0.9669590226f, -0.254931851f, -0.9792894595f, 0.2024651934f, -0.5369502995f, -0.8436138784f, -0.270036471f, -0.9628500944f, -0.6400277131f, 0.7683518247f,
    -0.7854537493f, -0.6189203566f, 0.06005905383f, -0.9981948257f, -0.02455770378f, 0.9996984141f, -0.65983623f, 0.751409442f, -0.6253894466f, -0.7803127835f, -0.6210408851f, -0.7837781695f, 0.8348888491f, 0.5504185768f, -0.1592275245f, 0.9872419133f,
    0.8367622488f, 0.5475663786f, -0.8675753916f, -0.4973056806f, -0.2022662628f, -0.9793305667f, 0.9399189937f, 0.3413975472f, 0.9877404807f, -0.1561049093f, -0.9034455656f, 0.4287028224f, 0.1269804218f, -0.9919052235f, -0.3819600854f, 0.924178821f,
    0.9754625894f, 0.2201652486f, -0.3204015856f, -0.9472818081f, -0.9874760884f, 0.1577687387f, 0.02535348474f, -0.9996785487f, 0.4835130794f, -0.8753371362f, -0.2850799925f, -0.9585037287f, -0.06805516006f, -0.99768156f, -0.7885244045f, -0.6150034663f,
    0.3185392127f, -0.9479096845f, 0.8880043089f, 0.4598351306f, 0.6476921488f, -0.7619021462f, 0.9820241299f, 0.1887554194f, 0.9357275128f, -0.3527237187f, -0.8894895414f, 0.4569555293f, 0.7922791302f, 0.6101588153f, 0.7483818261f, 0.6632681526f,
    -0.7288929755f, -0.6846276581f, 0.8729032783f, -0.4878932944f, 0.8288345784f, 0.5594937369f, 0.08074567077f, 0.9967347374f, 0.9799148216f, -0.1994165048f, -0.580730673f, -0.8140957471f, -0.4700049791f, -0.8826637636f, 0.2409492979f, 0.9705377045f,
    0.9437816757f, -0.3305694308f, -0.8927998638f, -0.4504535528f, -0.8069622304f, 0.5906030467f, 0.06258973166f, 0.9980393407f, -0.9312597469f, 0.3643559849f, 0.5777449785f, 0.8162173362f, -0.3360095855f, -0.941858566f, 0.697932075f, -0.7161639607f,
    -0.002008157227f, -0.9999979837f, -0.1827294312f, -0.9831632392f, -0.6523911722f, 0.7578824173f, -0.4302626911f, -0.9027037258f, -0.9985126289f, -0.05452091251f, -0.01028102172f, -0.9999471489f, -0.4946071129f, 0.8691166802f, -0.2999350194f, 0.9539596344f,
    0.8165471961f, 0.5772786819f, 0.2697460475f, 0.962931498f, -0.7306287391f, -0.6827749597f, -0.7590952064f, -0.6509796216f, -0.907053853f, 0.4210146171f, -0.5104861064f, -0.8598860013f, 0.8613350597f, 0.5080373165f, 0.5007881595f, -0.8655698812f,
    -0.654158152f, 0.7563577938f, -0.8382755311f, -0.545246856f, 0.6940070834f, 0.7199681717f, 0.06950936031f, 0.9975812994f, 0.1702942185f, -0.9853932612f, 0.2695973274f, 0.9629731466f, 0.5519612192f, -0.8338697815f, 0.225657487f, -0.9742067022f,
    0.4215262855f, -0.9068161835f, 0.4881873305f, -0.8727388672f, -0.3683854996f, -0.9296731273f, -0.9825390578f, 0.1860564427f, 0.81256471f, 0.5828709909f, 0.3196460933f, -0.9475370046f, 0.9570913859f, 0.2897862643f, -0.6876655497f, -0.7260276109f,
    -0.9988770922f, -0.047376731f, -0.1250179027f, 0.992154486f, -0.8280133617f, 0.560708367f, 0.9324863769f, -0.3612051451f, 0.6394653183f, 0.7688199442f, -0.01623847064f, -0.9998681473f, -0.9955014666f, -0.09474613458f, -0.81453315f, 0.580117012f,
    0.4037327978f, -0.9148769469f, 0.9944263371f, 0.1054336766f, -0.1624711654f, 0.9867132919f, -0.9949487814f, -0.100383875f, -0.6995302564f, 0.7146029809f, 0.5263414922f, -0.85027327f, -0.5395221479f, 0.841971408f, 0.6579370318f, 0.7530729462f,
    0.01426758847f, -0.9998982128f, -0.6734383991f, 0.7392433447f, 0.639412098f, -0.7688642071f, 0.9211571421f, 0.3891908523f, -0.146637214f, -0.9891903394f, -0.782318098f, 0.6228791163f, -0.5039610839f, -0.8637263605f, -0.7743120191f, -0.6328039957f
};

/* 256 (x,y,z,0) quadruplets, same fixed-4-stride trailing-0 shape as
 * simplex's 3D gradient table above and for the same reason (matches the
 * 2-bit-shifted hash mask below). */
static const f32 voronoi_rand_vecs_3d_[] = {
    -0.7292736885f, -0.6618439697f, 0.1735581948f, 0, 0.790292081f, -0.5480887466f, -0.2739291014f, 0, 0.7217578935f, 0.6226212466f, -0.3023380997f, 0, 0.565683137f, -0.8208298145f, -0.0790000257f, 0, 0.760049034f, -0.5555979497f, -0.3370999617f, 0, 0.3713945616f, 0.5011264475f, 0.7816254623f, 0, -0.1277062463f, -0.4254438999f, -0.8959289049f, 0, -0.2881560924f, -0.5815838982f, 0.7607405838f, 0,
    0.5849561111f, -0.662820239f, -0.4674352136f, 0, 0.3307171178f, 0.0391653737f, 0.94291689f, 0, 0.8712121778f, -0.4113374369f, -0.2679381538f, 0, 0.580981015f, 0.7021915846f, 0.4115677815f, 0, 0.503756873f, 0.6330056931f, -0.5878203852f, 0, 0.4493712205f, 0.601390195f, 0.6606022552f, 0, -0.6878403724f, 0.09018890807f, -0.7202371714f, 0, -0.5958956522f, -0.6469350577f, 0.475797649f, 0,
    -0.5127052122f, 0.1946921978f, -0.8361987284f, 0, -0.9911507142f, -0.05410276466f, -0.1212153153f, 0, -0.2149721042f, 0.9720882117f, -0.09397607749f, 0, -0.7518650936f, -0.5428057603f, 0.3742469607f, 0, 0.5237068895f, 0.8516377189f, -0.02107817834f, 0, 0.6333504779f, 0.1926167129f, -0.7495104896f, 0, -0.06788241606f, 0.3998305789f, 0.9140719259f, 0, -0.5538628599f, -0.4729896695f, -0.6852128902f, 0,
    -0.7261455366f, -0.5911990757f, 0.3509933228f, 0, -0.9229274737f, -0.1782808786f, 0.3412049336f, 0, -0.6968815002f, 0.6511274338f, 0.3006480328f, 0, 0.9608044783f, -0.2098363234f, -0.1811724921f, 0, 0.06817146062f, -0.9743405129f, 0.2145069156f, 0, -0.3577285196f, -0.6697087264f, -0.6507845481f, 0, -0.1868621131f, 0.7648617052f, -0.6164974636f, 0, -0.6541697588f, 0.3967914832f, 0.6439087246f, 0,
    0.6993340405f, -0.6164538506f, 0.3618239211f, 0, -0.1546665739f, 0.6291283928f, 0.7617583057f, 0, -0.6841612949f, -0.2580482182f, -0.6821542638f, 0, 0.5383980957f, 0.4258654885f, 0.7271630328f, 0, -0.5026987823f, -0.7939832935f, -0.3418836993f, 0, 0.3202971715f, 0.2834415347f, 0.9039195862f, 0, 0.8683227101f, -0.0003762656404f, -0.4959995258f, 0, 0.791120031f, -0.08511045745f, 0.6057105799f, 0,
    -0.04011016052f, -0.4397248749f, 0.8972364289f, 0, 0.9145119872f, 0.3579346169f, -0.1885487608f, 0, -0.9612039066f, -0.2756484276f, 0.01024666929f, 0, 0.6510361721f, -0.2877799159f, -0.7023778346f, 0, -0.2041786351f, 0.7365237271f, 0.644859585f, 0, -0.7718263711f, 0.3790626912f, 0.5104855816f, 0, -0.3060082741f, -0.7692987727f, 0.5608371729f, 0, 0.454007341f, -0.5024843065f, 0.7357899537f, 0,
    0.4816795475f, 0.6021208291f, -0.6367380315f, 0, 0.6961980369f, -0.3222197429f, 0.641469197f, 0, -0.6532160499f, -0.6781148932f, 0.3368515753f, 0, 0.5089301236f, -0.6154662304f, -0.6018234363f, 0, -0.1635919754f, -0.9133604627f, -0.372840892f, 0, 0.52408019f, -0.8437664109f, 0.1157505864f, 0, 0.5902587356f, 0.4983817807f, -0.6349883666f, 0, 0.5863227872f, 0.494764745f, 0.6414307729f, 0,
    0.6779335087f, 0.2341345225f, 0.6968408593f, 0, 0.7177054546f, -0.6858979348f, 0.120178631f, 0, -0.5328819713f, -0.5205125012f, 0.6671608058f, 0, -0.8654874251f, -0.0700727088f, -0.4960053754f, 0, -0.2861810166f, 0.7952089234f, 0.5345495242f, 0, -0.04849529634f, 0.9810836427f, -0.1874115585f, 0, -0.6358521667f, 0.6058348682f, 0.4781800233f, 0, 0.6254794696f, -0.2861619734f, 0.7258696564f, 0,
    -0.2585259868f, 0.5061949264f, -0.8227581726f, 0, 0.02136306781f, 0.5064016808f, -0.8620330371f, 0, 0.200111773f, 0.8599263484f, 0.4695550591f, 0, 0.4743561372f, 0.6014985084f, -0.6427953014f, 0, 0.6622993731f, -0.5202474575f, -0.5391679918f, 0, 0.08084972818f, -0.6532720452f, 0.7527940996f, 0, -0.6893687501f, 0.0592860349f, 0.7219805347f, 0, -0.1121887082f, -0.9673185067f, 0.2273952515f, 0,
    0.7344116094f, 0.5979668656f, -0.3210532909f, 0, 0.5789393465f, -0.2488849713f, 0.7764570201f, 0, 0.6988182827f, 0.3557169806f, -0.6205791146f, 0, -0.8636845529f, -0.2748771249f, -0.4224826141f, 0, -0.4247027957f, -0.4640880967f, 0.777335046f, 0, 0.5257722489f, -0.8427017621f, 0.1158329937f, 0, 0.9343830603f, 0.316302472f, -0.1639543925f, 0, -0.1016836419f, -0.8057303073f, -0.5834887393f, 0,
    -0.6529238969f, 0.50602126f, -0.5635892736f, 0, -0.2465286165f, -0.9668205684f, -0.06694497494f, 0, -0.9776897119f, -0.2099250524f, -0.007368825344f, 0, 0.7736893337f, 0.5734244712f, 0.2694238123f, 0, -0.6095087895f, 0.4995678998f, 0.6155736747f, 0, 0.5794535482f, 0.7434546771f, 0.3339292269f, 0, -0.8226211154f, 0.08142581855f, 0.5627293636f, 0, -0.510385483f, 0.4703667658f, 0.7199039967f, 0,
    -0.5764971849f, -0.07231656274f, -0.8138926898f, 0, 0.7250628871f, 0.3949971505f, -0.5641463116f, 0, -0.1525424005f, 0.4860840828f, -0.8604958341f, 0, -0.5550976208f, -0.4957820792f, 0.667882296f, 0, -0.1883614327f, 0.9145869398f, 0.357841725f, 0, 0.7625556724f, -0.5414408243f, -0.3540489801f, 0, -0.5870231946f, -0.3226498013f, -0.7424963803f, 0, 0.3051124198f, 0.2262544068f, -0.9250488391f, 0,
    0.6379576059f, 0.577242424f, -0.5097070502f, 0, -0.5966775796f, 0.1454852398f, -0.7891830656f, 0, -0.658330573f, 0.6555487542f, -0.3699414651f, 0, 0.7434892426f, 0.2351084581f, 0.6260573129f, 0, 0.5562114096f, 0.8264360377f, -0.0873632843f, 0, -0.3028940016f, -0.8251527185f, 0.4768419182f, 0, 0.1129343818f, -0.985888439f, -0.1235710781f, 0, 0.5937652891f, -0.5896813806f, 0.5474656618f, 0,
    0.6757964092f, -0.5835758614f, -0.4502648413f, 0, 0.7242302609f, -0.1152719764f, 0.6798550586f, 0, -0.9511914166f, 0.0753623979f, -0.2992580792f, 0, 0.2539470961f, -0.1886339355f, 0.9486454084f, 0, 0.571433621f, -0.1679450851f, -0.8032795685f, 0, -0.06778234979f, 0.3978269256f, 0.9149531629f, 0, 0.6074972649f, 0.733060024f, -0.3058922593f, 0, -0.5435478392f, 0.1675822484f, 0.8224791405f, 0,
    -0.5876678086f, -0.3380045064f, -0.7351186982f, 0, -0.7967562402f, 0.04097822706f, -0.6029098428f, 0, -0.1996350917f, 0.8706294745f, 0.4496111079f, 0, -0.02787660336f, -0.9106232682f, -0.4122962022f, 0, -0.7797625996f, -0.6257634692f, 0.01975775581f, 0, -0.5211232846f, 0.7401644346f, -0.4249554471f, 0, 0.8575424857f, 0.4053272873f, -0.3167501783f, 0, 0.1045223322f, 0.8390195772f, -0.5339674439f, 0,
    0.3501822831f, 0.9242524096f, -0.1520850155f, 0, 0.1987849858f, 0.07647613266f, 0.9770547224f, 0, 0.7845996363f, 0.6066256811f, -0.1280964233f, 0, 0.09006737436f, -0.9750989929f, -0.2026569073f, 0, -0.8274343547f, -0.542299559f, 0.1458203587f, 0, -0.3485797732f, -0.415802277f, 0.840000362f, 0, -0.2471778936f, -0.7304819962f, -0.6366310879f, 0, -0.3700154943f, 0.8577948156f, 0.3567584454f, 0,
    0.5913394901f, -0.548311967f, -0.5913303597f, 0, 0.1204873514f, -0.7626472379f, -0.6354935001f, 0, 0.616959265f, 0.03079647928f, 0.7863922953f, 0, 0.1258156836f, -0.6640829889f, -0.7369967419f, 0, -0.6477565124f, -0.1740147258f, -0.7417077429f, 0, 0.6217889313f, -0.7804430448f, -0.06547655076f, 0, 0.6589943422f, -0.6096987708f, 0.4404473475f, 0, -0.2689837504f, -0.6732403169f, -0.6887635427f, 0,
    -0.3849775103f, 0.5676542638f, 0.7277093879f, 0, 0.5754444408f, 0.8110471154f, -0.1051963504f, 0, 0.9141593684f, 0.3832947817f, 0.131900567f, 0, -0.107925319f, 0.9245493968f, 0.3654593525f, 0, 0.377977089f, 0.3043148782f, 0.8743716458f, 0, -0.2142885215f, -0.8259286236f, 0.5214617324f, 0, 0.5802544474f, 0.4148098596f, -0.7008834116f, 0, -0.1982660881f, 0.8567161266f, -0.4761596756f, 0,
    -0.03381553704f, 0.3773180787f, -0.9254661404f, 0, -0.6867922841f, -0.6656597827f, 0.2919133642f, 0, 0.7731742607f, -0.2875793547f, -0.5652430251f, 0, -0.09655941928f, 0.9193708367f, -0.3813575004f, 0, 0.2715702457f, -0.9577909544f, -0.09426605581f, 0, 0.2451015704f, -0.6917998565f, -0.6792188003f, 0, 0.977700782f, -0.1753855374f, 0.1155036542f, 0, -0.5224739938f, 0.8521606816f, 0.02903615945f, 0,
    -0.7734880599f, -0.5261292347f, 0.3534179531f, 0, -0.7134492443f, -0.269547243f, 0.6467878011f, 0, 0.1644037271f, 0.5105846203f, -0.8439637196f, 0, 0.6494635788f, 0.05585611296f, 0.7583384168f, 0, -0.4711970882f, 0.5017280509f, -0.7254255765f, 0, -0.6335764307f, -0.2381686273f, -0.7361091029f, 0, -0.9021533097f, -0.270947803f, -0.3357181763f, 0, -0.3793711033f, 0.872258117f, 0.3086152025f, 0,
    -0.6855598966f, -0.3250143309f, 0.6514394162f, 0, 0.2900942212f, -0.7799057743f, -0.5546100667f, 0, -0.2098319339f, 0.85037073f, 0.4825351604f, 0, -0.4592603758f, 0.6598504336f, -0.5947077538f, 0, 0.8715945488f, 0.09616365406f, -0.4807031248f, 0, -0.6776666319f, 0.7118504878f, -0.1844907016f, 0, 0.7044377633f, 0.312427597f, 0.637304036f, 0, -0.7052318886f, -0.2401093292f, -0.6670798253f, 0,
    0.081921007f, -0.7207336136f, -0.6883545647f, 0, -0.6993680906f, -0.5875763221f, -0.4069869034f, 0, -0.1281454481f, 0.6419895885f, 0.7559286424f, 0, -0.6337388239f, -0.6785471501f, -0.3714146849f, 0, 0.5565051903f, -0.2168887573f, -0.8020356851f, 0, -0.5791554484f, 0.7244372011f, -0.3738578718f, 0, 0.1175779076f, -0.7096451073f, 0.6946792478f, 0, -0.6134619607f, 0.1323631078f, 0.7785527795f, 0,
    0.6984635305f, -0.02980516237f, -0.715024719f, 0, 0.8318082963f, -0.3930171956f, 0.3919597455f, 0, 0.1469576422f, 0.05541651717f, -0.9875892167f, 0, 0.708868575f, -0.2690503865f, 0.6520101478f, 0, 0.2726053183f, 0.67369766f, -0.68688995f, 0, -0.6591295371f, 0.3035458599f, -0.6880466294f, 0, 0.4815131379f, -0.7528270071f, 0.4487723203f, 0, 0.9430009463f, 0.1675647412f, -0.2875261255f, 0,
    0.434802957f, 0.7695304522f, -0.4677277752f, 0, 0.3931996188f, 0.594473625f, 0.7014236729f, 0, 0.7254336655f, -0.603925654f, 0.3301814672f, 0, 0.7590235227f, -0.6506083235f, 0.02433313207f, 0, -0.8552768592f, -0.3430042733f, 0.3883935666f, 0, -0.6139746835f, 0.6981725247f, 0.3682257648f, 0, -0.7465905486f, -0.5752009504f, 0.3342849376f, 0, 0.5730065677f, 0.810555537f, -0.1210916791f, 0,
    -0.9225877367f, -0.3475211012f, -0.167514036f, 0, -0.7105816789f, -0.4719692027f, -0.5218416899f, 0, -0.08564609717f, 0.3583001386f, 0.929669703f, 0, -0.8279697606f, -0.2043157126f, 0.5222271202f, 0, 0.427944023f, 0.278165994f, 0.8599346446f, 0, 0.5399079671f, -0.7857120652f, -0.3019204161f, 0, 0.5678404253f, -0.5495413974f, -0.6128307303f, 0, -0.9896071041f, 0.1365639107f, -0.04503418428f, 0,
    -0.6154342638f, -0.6440875597f, 0.4543037336f, 0, 0.1074204368f, -0.7946340692f, 0.5975094525f, 0, -0.3595449969f, -0.8885529948f, 0.28495784f, 0, -0.2180405296f, 0.1529888965f, 0.9638738118f, 0, -0.7277432317f, -0.6164050508f, -0.3007234646f, 0, 0.7249729114f, -0.00669719484f, 0.6887448187f, 0, -0.5553659455f, -0.5336586252f, 0.6377908264f, 0, 0.5137558015f, 0.7976208196f, -0.3160000073f, 0,
    -0.3794024848f, 0.9245608561f, -0.03522751494f, 0, 0.8229248658f, 0.2745365933f, -0.4974176556f, 0, -0.5404114394f, 0.6091141441f, 0.5804613989f, 0, 0.8036581901f, -0.2703029469f, 0.5301601931f, 0, 0.6044318879f, 0.6832968393f, 0.4095943388f, 0, 0.06389988817f, 0.9658208605f, -0.2512108074f, 0, 0.1087113286f, 0.7402471173f, -0.6634877936f, 0, -0.713427712f, -0.6926784018f, 0.1059128479f, 0,
    0.6458897819f, -0.5724548511f, -0.5050958653f, 0, -0.6553931414f, 0.7381471625f, 0.159995615f, 0, 0.3910961323f, 0.9188871375f, -0.05186755998f, 0, -0.4879022471f, -0.5904376907f, 0.6429111375f, 0, 0.6014790094f, 0.7707441366f, -0.2101820095f, 0, -0.5677173047f, 0.7511360995f, 0.3368851762f, 0, 0.7858573506f, 0.226674665f, 0.5753666838f, 0, -0.4520345543f, -0.604222686f, -0.6561857263f, 0,
    0.002272116345f, 0.4132844051f, -0.9105991643f, 0, -0.5815751419f, -0.5162925989f, 0.6286591339f, 0, -0.03703704785f, 0.8273785755f, 0.5604221175f, 0, -0.5119692504f, 0.7953543429f, -0.3244980058f, 0, -0.2682417366f, -0.9572290247f, -0.1084387619f, 0, -0.2322482736f, -0.9679131102f, -0.09594243324f, 0, 0.3554328906f, -0.8881505545f, 0.2913006227f, 0, 0.7346520519f, -0.4371373164f, 0.5188422971f, 0,
    0.9985120116f, 0.04659011161f, -0.02833944577f, 0, -0.3727687496f, -0.9082481361f, 0.1900757285f, 0, 0.91737377f, -0.3483642108f, 0.1925298489f, 0, 0.2714911074f, 0.4147529736f, -0.8684886582f, 0, 0.5131763485f, -0.7116334161f, 0.4798207128f, 0, -0.8737353606f, 0.18886992f, -0.4482350644f, 0, 0.8460043821f, -0.3725217914f, 0.3814499973f, 0, 0.8978727456f, -0.1780209141f, -0.4026575304f, 0,
    0.2178065647f, -0.9698322841f, -0.1094789531f, 0, -0.1518031304f, -0.7788918132f, -0.6085091231f, 0, -0.2600384876f, -0.4755398075f, -0.8403819825f, 0, 0.572313509f, -0.7474340931f, -0.3373418503f, 0, -0.7174141009f, 0.1699017182f, -0.6756111411f, 0, -0.684180784f, 0.02145707593f, -0.7289967412f, 0, -0.2007447902f, 0.06555605789f, -0.9774476623f, 0, -0.1148803697f, -0.8044887315f, 0.5827524187f, 0,
    -0.7870349638f, 0.03447489231f, 0.6159443543f, 0, -0.2015596421f, 0.6859872284f, 0.6991389226f, 0, -0.08581082512f, -0.10920836f, -0.9903080513f, 0, 0.5532693395f, 0.7325250401f, -0.396610771f, 0, -0.1842489331f, -0.9777375055f, -0.1004076743f, 0, 0.0775473789f, -0.9111505856f, 0.4047110257f, 0, 0.1399838409f, 0.7601631212f, -0.6344734459f, 0, 0.4484419361f, -0.845289248f, 0.2904925424f, 0
};

static i32 voronoi_hash2d_(i32 seed, u32 xp, u32 yp)
{
    u32 hash = (u32)seed ^ xp ^ yp;
    hash *= 0x27d4eb2du;
    return (i32)hash;
}

static i32 voronoi_hash3d_(i32 seed, u32 xp, u32 yp, u32 zp)
{
    u32 hash = (u32)seed ^ xp ^ yp ^ zp;
    hash *= 0x27d4eb2du;
    return (i32)hash;
}

static i32 voronoi_round_(f32 f) { return (f >= 0.0f) ? (i32)(f + 0.5f) : (i32)(f - 0.5f); }

/* the if/elif chains below (in place of upstream's
 * `distance1 = max(min(distance1, new), distance0)`, run unconditionally
 * every iteration) are an ordinary top-2-smallest tracker - algebraically
 * identical to upstream's formula in all three cases (new closer than
 * both / between the two / farther than both), verified by hand while
 * porting, just without the redundant min/max calls on iterations that
 * don't change anything. */
void voronoi_query_2d(i32 seed, f32 x, f32 y, f32 jitter, Voronoi_Query *out)
{
    i32 xr = voronoi_round_(x);
    i32 yr = voronoi_round_(y);
    f32 distance0 = VORONOI_FLT_MAX_;
    f32 distance1 = VORONOI_FLT_MAX_;
    i32 closest_hash = 0;
    f32 cellular_jitter = 0.43701595f * jitter;
    u32 x_primed_base = (u32)(xr - 1) * voronoi_prime_x_;
    u32 y_primed_base = (u32)(yr - 1) * voronoi_prime_y_;
    i32 xi, yi;
    u32 x_primed = x_primed_base;

    for (xi = xr - 1; xi <= xr + 1; xi++) {
        u32 y_primed = y_primed_base;
        for (yi = yr - 1; yi <= yr + 1; yi++) {
            i32 hash = voronoi_hash2d_(seed, x_primed, y_primed);
            i32 idx = hash & (255 << 1);
            f32 vec_x = ((f32)xi - x) + voronoi_rand_vecs_2d_[idx] * cellular_jitter;
            f32 vec_y = ((f32)yi - y) + voronoi_rand_vecs_2d_[idx | 1] * cellular_jitter;
            f32 new_distance = vec_x * vec_x + vec_y * vec_y;

            if (new_distance < distance0) {
                distance1 = distance0;
                distance0 = new_distance;
                closest_hash = hash;
            } else if (new_distance < distance1) {
                distance1 = new_distance;
            }
            y_primed += voronoi_prime_y_;
        }
        x_primed += voronoi_prime_x_;
    }

    out->distance0 = (f32)voronoi_sqrt_((f64)distance0);
    out->distance1 = (f32)voronoi_sqrt_((f64)distance1);
    out->closest_hash = closest_hash;
}

void voronoi_query_3d(i32 seed, f32 x, f32 y, f32 z, f32 jitter, Voronoi_Query *out)
{
    i32 xr = voronoi_round_(x);
    i32 yr = voronoi_round_(y);
    i32 zr = voronoi_round_(z);
    f32 distance0 = VORONOI_FLT_MAX_;
    f32 distance1 = VORONOI_FLT_MAX_;
    i32 closest_hash = 0;
    f32 cellular_jitter = 0.39614353f * jitter;
    u32 x_primed_base = (u32)(xr - 1) * voronoi_prime_x_;
    u32 y_primed_base = (u32)(yr - 1) * voronoi_prime_y_;
    u32 z_primed_base = (u32)(zr - 1) * voronoi_prime_z_;
    i32 xi, yi, zi;
    u32 x_primed = x_primed_base;

    for (xi = xr - 1; xi <= xr + 1; xi++) {
        u32 y_primed = y_primed_base;
        for (yi = yr - 1; yi <= yr + 1; yi++) {
            u32 z_primed = z_primed_base;
            for (zi = zr - 1; zi <= zr + 1; zi++) {
                i32 hash = voronoi_hash3d_(seed, x_primed, y_primed, z_primed);
                i32 idx = hash & (255 << 2);
                f32 vec_x = ((f32)xi - x) + voronoi_rand_vecs_3d_[idx] * cellular_jitter;
                f32 vec_y = ((f32)yi - y) + voronoi_rand_vecs_3d_[idx | 1] * cellular_jitter;
                f32 vec_z = ((f32)zi - z) + voronoi_rand_vecs_3d_[idx | 2] * cellular_jitter;
                f32 new_distance = vec_x * vec_x + vec_y * vec_y + vec_z * vec_z;

                if (new_distance < distance0) {
                    distance1 = distance0;
                    distance0 = new_distance;
                    closest_hash = hash;
                } else if (new_distance < distance1) {
                    distance1 = new_distance;
                }
                z_primed += voronoi_prime_z_;
            }
            y_primed += voronoi_prime_y_;
        }
        x_primed += voronoi_prime_x_;
    }

    out->distance0 = (f32)voronoi_sqrt_((f64)distance0);
    out->distance1 = (f32)voronoi_sqrt_((f64)distance1);
    out->closest_hash = closest_hash;
}

f32 voronoi_cell_value(const Voronoi_Query *q)
{
    return (f32)q->closest_hash * (1.0f / 2147483648.0f);
}

f32 voronoi_edge_2d(i32 seed, f32 x, f32 y, f32 jitter)
{
    Voronoi_Query q;
    voronoi_query_2d(seed, x, y, jitter, &q);
    return q.distance1 - q.distance0;
}

f32 voronoi_edge_3d(i32 seed, f32 x, f32 y, f32 z, f32 jitter)
{
    Voronoi_Query q;
    voronoi_query_3d(seed, x, y, z, jitter, &q);
    return q.distance1 - q.distance0;
}

#endif /* VORONOI_IMPLEMENTATION_ */
#endif /* VORONOI_IMPLEMENTATION */
