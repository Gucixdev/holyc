#ifndef HOLYC_NN_H_
#define HOLYC_NN_H_

/* From-scratch feedforward MLP + backprop. C89 port of tsoding's nn.h
 * (https://github.com/tsoding/nn.h, MIT license) - the algorithms and
 * overall Mat/Row/NN shape are faithfully theirs, the C99 surface is
 * swapped for this project's own:
 *
 *   - nn.h's own bespoke "Region" bump allocator -> this project's Arena.
 *     They're the same idea down to the API shape - region_save/
 *     region_rewind IS arena_snapshot/arena_rewind, word-aligned bump
 *     allocation is exactly what arena_alloc already does. No reason to
 *     carry a second, slightly different allocator just for this module.
 *   - designated initializers, compound literals, mid-loop `size_t i`
 *     declarations, stdbool.h `bool` -> plain C89 forms (Batch.finished
 *     becomes i32 0/1, matching how the rest of this project already
 *     avoids a bool type in favor of plain ints - see base_type.h's own
 *     comment on that)
 *   - printf-based mat_print/nn_print -> this project's own print.h
 *     ("own print without printf" is one of the design rules, not just true of
 *     core/print.h's own callers)
 *   - libc rand()/RAND_MAX -> a small seedable xorshift32, so a training
 *     run is reproducible instead of tangled up with whatever rand()
 *     happens to be seeded to on a given run
 *   - tsoding's own tanhf ported as-is (already hand-rolled via two
 *     expf() calls upstream, not a libm call to begin with); exp/sin/
 *     cos below are this project's own from-scratch versions instead
 *     of libm's expf/sinf/cosf, matching base.h's own vector_sqrt_/
 *     quat_sin_/quat_cos_ pattern (this project's "0 glibc" push,
 *     no-glibc rule) - a stance this file
 *     used to explicitly NOT take (an earlier version of this comment
 *     said the opposite), reversed once "0 glibc" became the actual
 *     goal rather than "don't reinvent libm badly" being the priority
 *
 * What's unchanged: the algorithms themselves (matrix-based forward pass,
 * both backprop variants gated by NN_BACKPROP_TRADITIONAL, finite-
 * difference gradient checking as an independent correctness cross-check
 * against backprop, mini-batch training via batch_process). No reason to
 * "improve" a design that already works and is this compact - port it
 * faithfully, extend later if something concrete needs it (bitnet/sparse/
 * attention are still planned, this is the
 * foundation under all of that, not a replacement for any of it). */

#include "base.h"
#include "print.h"
#include "arena_alloc.h"

#ifndef NN_ACT
#define NN_ACT ACT_SIG
#endif

#ifndef NN_RELU_PARAM
#define NN_RELU_PARAM 0.01f
#endif

#define ACT_SIG  0
#define ACT_RELU 1
#define ACT_TANH 2
#define ACT_SIN  3
typedef i32 Act;

/* ---- xorshift32: small, seedable, deterministic - not libc rand() ---- */

typedef struct { u32 state; } Nn_Rng;

void nn_rng_seed(Nn_Rng *rng, u32 seed);
u32  nn_rng_u32(Nn_Rng *rng);
f32  nn_rng_f32(Nn_Rng *rng); /* [0, 1) */

f32 sigmoidf_(f32 x);
f32 reluf_(f32 x);
f32 actf_(f32 x, Act act);
f32 dactf_(f32 y, Act act); /* derivative, expressed in terms of the activation's own output */

/* ---- Mat / Row: both views over a flat f32 buffer ---- */

typedef struct {
    size_t rows;
    size_t cols;
    f32 *elements;
} Mat;

typedef struct {
    size_t cols;
    f32 *elements;
} Row;

#define MAT_AT(m, i, j) (m).elements[(i) * (m).cols + (j)]
#define ROW_AT(row, col) (row).elements[col]

Mat mat_alloc(Arena *a, size_t rows, size_t cols);
void mat_fill(Mat m, f32 x);
void mat_rand(Nn_Rng *rng, Mat m, f32 low, f32 high);
Row mat_row(Mat m, size_t row);
void mat_copy(Mat dst, Mat src);
void mat_dot(Mat dst, Mat a, Mat b);
void mat_sum(Mat dst, Mat a);
void mat_act(Mat m);
void mat_print(Mat m, const char *name, size_t padding);
void mat_shuffle_rows(Nn_Rng *rng, Mat m);

Mat row_as_mat(Row row);
Row row_alloc(Arena *a, size_t cols);
Row row_slice(Row row, size_t i, size_t cols);
void row_rand(Nn_Rng *rng, Row row, f32 low, f32 high);
void row_fill(Row row, f32 x);
void row_print(Row row, const char *name, size_t padding);
void row_copy(Row dst, Row src);

/* ---- NN: a stack of (weights, bias, activation) layers ---- */

typedef struct {
    size_t *arch;
    size_t arch_count;
    Mat *ws;  /* arch_count - 1 weight matrices */
    Row *bs;  /* arch_count - 1 bias rows */
    Row *as;  /* arch_count activation rows (as[0] is the input) */
} NN;

#define NN_INPUT(nn)  ((nn).as[0])
#define NN_OUTPUT(nn) ((nn).as[(nn).arch_count - 1])

NN nn_alloc(Arena *a, size_t *arch, size_t arch_count);
void nn_zero(NN nn);
void nn_print(NN nn, const char *name);
void nn_rand(Nn_Rng *rng, NN nn, f32 low, f32 high);
void nn_forward(NN nn);
f32 nn_cost(NN nn, Mat t);
NN nn_finite_diff(Arena *a, NN nn, Mat t, f32 eps);
NN nn_backprop(Arena *a, NN nn, Mat t);
void nn_learn(NN nn, NN g, f32 rate);

typedef struct {
    size_t begin;
    f32 cost;
    i32 finished;
} Batch;

void batch_process(Arena *a, Batch *b, size_t batch_size, NN nn, Mat t, f32 rate);

#endif /* HOLYC_NN_H_ */

#ifdef NN_IMPLEMENTATION
#ifndef NN_IMPLEMENTATION_
#define NN_IMPLEMENTATION_

void nn_rng_seed(Nn_Rng *rng, u32 seed)
{
    /* xorshift is undefined at state 0 - never let it land there. seed|1
     * only when seed==0 leaves any nonzero seed untouched and maps 0->1. */
    rng->state = seed | (u32)(seed == 0);
}

u32 nn_rng_u32(Nn_Rng *rng)
{
    u32 x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

f32 nn_rng_f32(Nn_Rng *rng)
{
    return (f32)nn_rng_u32(rng) / (f32)4294967295.0;
}

/* From-scratch exp/sin/cos/sqrt, replacing math.h's - own local
 * copies, matching base.h's own vector_sqrt_/quat_sin_/quat_cos_
 * pattern (0 glibc push). exp(x) works by range-reducing x = k*ln2 + r
 * (r bounded to [-ln2/2, ln2/2], so a short Taylor series converges
 * fast), then scaling the result by 2^k via direct IEEE-754 exponent-
 * field manipulation (union-based type pun, the same sanctioned GCC/
 * Clang trick quake's famous fast-inverse-sqrt uses for f32) rather
 * than a real multiply. */
static f64 nn_exp_(f64 x)
{
    union { f64 f; u64 u; } c;
    const f64 ln2 = 0.69314718055994530942;
    i64 k;
    f64 r, term, sum;
    i32 n;
    u64 bits, exp_field, rest;

    k = (i64)(x / ln2 + ((x >= 0.0) ? 0.5 : -0.5));
    r = x - (f64)k * ln2;

    term = 1.0;
    sum = 1.0;
    for (n = 1; n <= 12; n++) {
        term *= r / (f64)n;
        sum += term;
    }

    c.f = sum;
    bits = c.u;
    exp_field = (bits >> 52) & 0x7ffUL;
    rest = bits & 0x800fffffffffffffUL;
    c.u = ((exp_field + (u64)k) << 52) | rest;
    return c.f;
}

static f64 nn_mod_2pi_(f64 x)
{
    const f64 two_pi = 6.283185307179586476925286766559;
    const f64 pi = 3.14159265358979323846;
    f64 n = (f64)(i64)(x / two_pi);
    f64 r = x - n * two_pi;
    if (r > pi) r -= two_pi;
    if (r < -pi) r += two_pi;
    return r;
}

static f64 nn_sin_(f64 x)
{
    f64 r = nn_mod_2pi_(x);
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

static f64 nn_tanh_(f64 x)
{
    f64 e2x;
    if (x > 20.0) return 1.0;   /* already 1.0 to float precision; avoids exp() overflow */
    if (x < -20.0) return -1.0;
    e2x = nn_exp_(2.0 * x);
    return (e2x - 1.0) / (e2x + 1.0);
}

static f64 nn_sqrt_(f64 x)
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

f32 sigmoidf_(f32 x)
{
    return 1.0f / (1.0f + (f32)nn_exp_((f64)(-x)));
}

f32 reluf_(f32 x)
{
    f32 pos = (f32)(x > 0.0f);
    return pos * x + (1.0f - pos) * (x * NN_RELU_PARAM);
}

f32 actf_(f32 x, Act act)
{
    switch (act) {
        case ACT_SIG:  return sigmoidf_(x);
        case ACT_RELU: return reluf_(x);
        case ACT_TANH: return (f32)nn_tanh_((f64)x);
        case ACT_SIN:  return (f32)nn_sin_((f64)x);
        default: break;
    }
    ARENA_ASSERT(0 && "actf_: unreachable");
    return 0.0f;
}

f32 dactf_(f32 y, Act act)
{
    switch (act) {
        case ACT_SIG:  return y * (1.0f - y);
        case ACT_RELU: return NN_RELU_PARAM + (1.0f - NN_RELU_PARAM) * (f32)(y >= 0.0f);
        case ACT_TANH: return 1.0f - y * y;
        /* cos(asin(y)) == sqrt(1-y^2) exactly: asin's principal branch is
         * [-pi/2, pi/2], where cos is always >= 0, so this isn't an
         * approximation of the old asin-then-cos call, it's the same
         * value via an algebraic identity that needs no asin at all. */
        case ACT_SIN:  return (f32)nn_sqrt_(1.0 - (f64)y * (f64)y);
        default: break;
    }
    ARENA_ASSERT(0 && "dactf_: unreachable");
    return 0.0f;
}

Mat mat_alloc(Arena *a, size_t rows, size_t cols)
{
    Mat m;
    m.rows = rows;
    m.cols = cols;
    m.elements = (f32 *)arena_alloc(a, sizeof(f32) * rows * cols);
    return m;
}

void mat_fill(Mat m, f32 x)
{
    size_t i, j;
    for (i = 0; i < m.rows; i++) {
        for (j = 0; j < m.cols; j++) {
            MAT_AT(m, i, j) = x;
        }
    }
}

void mat_rand(Nn_Rng *rng, Mat m, f32 low, f32 high)
{
    size_t i, j;
    for (i = 0; i < m.rows; i++) {
        for (j = 0; j < m.cols; j++) {
            MAT_AT(m, i, j) = nn_rng_f32(rng) * (high - low) + low;
        }
    }
}

Row mat_row(Mat m, size_t row)
{
    Row r;
    r.cols = m.cols;
    r.elements = &MAT_AT(m, row, 0);
    return r;
}

void mat_copy(Mat dst, Mat src)
{
    size_t i, j;
    ARENA_ASSERT(dst.rows == src.rows);
    ARENA_ASSERT(dst.cols == src.cols);
    for (i = 0; i < dst.rows; i++) {
        for (j = 0; j < dst.cols; j++) {
            MAT_AT(dst, i, j) = MAT_AT(src, i, j);
        }
    }
}

void mat_dot(Mat dst, Mat a, Mat b)
{
    size_t i, j, k, n;
    ARENA_ASSERT(a.cols == b.rows);
    n = a.cols;
    ARENA_ASSERT(dst.rows == a.rows);
    ARENA_ASSERT(dst.cols == b.cols);

    for (i = 0; i < dst.rows; i++) {
        for (j = 0; j < dst.cols; j++) {
            MAT_AT(dst, i, j) = 0.0f;
            for (k = 0; k < n; k++) {
                MAT_AT(dst, i, j) += MAT_AT(a, i, k) * MAT_AT(b, k, j);
            }
        }
    }
}

void mat_sum(Mat dst, Mat a)
{
    size_t i, j;
    ARENA_ASSERT(dst.rows == a.rows);
    ARENA_ASSERT(dst.cols == a.cols);
    for (i = 0; i < dst.rows; i++) {
        for (j = 0; j < dst.cols; j++) {
            MAT_AT(dst, i, j) += MAT_AT(a, i, j);
        }
    }
}

void mat_act(Mat m)
{
    size_t i, j;
    for (i = 0; i < m.rows; i++) {
        for (j = 0; j < m.cols; j++) {
            MAT_AT(m, i, j) = actf_(MAT_AT(m, i, j), NN_ACT);
        }
    }
}

void mat_print(Mat m, const char *name, size_t padding)
{
    size_t i, j, p;
    for (p = 0; p < padding; p++) print_char(' ');
    print_cstr(name); print_cstr(" = [\n");
    for (i = 0; i < m.rows; i++) {
        for (p = 0; p < padding; p++) print_char(' ');
        print_cstr("    ");
        for (j = 0; j < m.cols; j++) {
            /* print.h has no float formatting - print a fixed-point
             * approximation (3 decimal digits) via the integer path
             * rather than pulling in a float-to-string routine for a
             * debug dump */
            f32 v = MAT_AT(m, i, j);
            i32 whole = (i32)v;
            f32 frac = v - (f32)whole;
            i32 milli = (i32)(frac * 1000.0f);
            i32 sign_mask = milli >> 31; /* all-0s or all-1s from the sign bit */
            milli = (milli + sign_mask) ^ sign_mask; /* branchless abs */
            print_i64(whole); print_char('.');
            if (milli < 100) print_char('0');
            if (milli < 10) print_char('0');
            print_i64(milli);
            print_char(' ');
        }
        print_char('\n');
    }
    for (p = 0; p < padding; p++) print_char(' ');
    print_cstr("]\n");
}

void mat_shuffle_rows(Nn_Rng *rng, Mat m)
{
    size_t i, j, k;
    for (i = 0; i < m.rows; i++) {
        j = i + (size_t)(nn_rng_u32(rng) % (u32)(m.rows - i));
        if (i != j) {
            for (k = 0; k < m.cols; k++) {
                f32 t = MAT_AT(m, i, k);
                MAT_AT(m, i, k) = MAT_AT(m, j, k);
                MAT_AT(m, j, k) = t;
            }
        }
    }
}

Mat row_as_mat(Row row)
{
    Mat m;
    m.rows = 1;
    m.cols = row.cols;
    m.elements = row.elements;
    return m;
}

Row row_alloc(Arena *a, size_t cols)
{
    return mat_row(mat_alloc(a, 1, cols), 0);
}

Row row_slice(Row row, size_t i, size_t cols)
{
    Row r;
    ARENA_ASSERT(i < row.cols);
    ARENA_ASSERT(i + cols <= row.cols);
    r.cols = cols;
    r.elements = &ROW_AT(row, i);
    return r;
}

void row_rand(Nn_Rng *rng, Row row, f32 low, f32 high) { mat_rand(rng, row_as_mat(row), low, high); }
void row_fill(Row row, f32 x)                          { mat_fill(row_as_mat(row), x); }
void row_print(Row row, const char *name, size_t padding) { mat_print(row_as_mat(row), name, padding); }
void row_copy(Row dst, Row src)                        { mat_copy(row_as_mat(dst), row_as_mat(src)); }

NN nn_alloc(Arena *a, size_t *arch, size_t arch_count)
{
    NN nn;
    size_t i;

    ARENA_ASSERT(arch_count > 0);

    nn.arch = arch;
    nn.arch_count = arch_count;
    nn.ws = (Mat *)arena_alloc(a, sizeof(*nn.ws) * (arch_count - 1));
    nn.bs = (Row *)arena_alloc(a, sizeof(*nn.bs) * (arch_count - 1));
    nn.as = (Row *)arena_alloc(a, sizeof(*nn.as) * arch_count);

    nn.as[0] = row_alloc(a, arch[0]);
    for (i = 1; i < arch_count; i++) {
        nn.ws[i - 1] = mat_alloc(a, nn.as[i - 1].cols, arch[i]);
        nn.bs[i - 1] = row_alloc(a, arch[i]);
        nn.as[i] = row_alloc(a, arch[i]);
    }

    return nn;
}

void nn_zero(NN nn)
{
    size_t i;
    for (i = 0; i < nn.arch_count - 1; i++) {
        mat_fill(nn.ws[i], 0.0f);
        row_fill(nn.bs[i], 0.0f);
        row_fill(nn.as[i], 0.0f);
    }
    row_fill(nn.as[nn.arch_count - 1], 0.0f);
}

void nn_print(NN nn, const char *name)
{
    size_t i;
    char buf[32];
    print_cstr(name); print_cstr(" = [\n");
    for (i = 0; i < nn.arch_count - 1; i++) {
        /* own tiny "sprintf" for exactly "ws%zu"/"bs%zu" - not worth a
         * general string-building facility for two call sites */
        size_t n = 0;
        buf[n++] = 'w'; buf[n++] = 's';
        {
            char digits[24];
            size_t dn = 0, v = i;
            if (v == 0) digits[dn++] = '0';
            while (v > 0) { digits[dn++] = (char)('0' + v % 10); v /= 10; }
            while (dn > 0) buf[n++] = digits[--dn];
        }
        buf[n] = '\0';
        mat_print(nn.ws[i], buf, 4);

        buf[0] = 'b';
        mat_print(row_as_mat(nn.bs[i]), buf, 4);
    }
    print_cstr("]\n");
}

void nn_rand(Nn_Rng *rng, NN nn, f32 low, f32 high)
{
    size_t i;
    for (i = 0; i < nn.arch_count - 1; i++) {
        mat_rand(rng, nn.ws[i], low, high);
        row_rand(rng, nn.bs[i], low, high);
    }
}

void nn_forward(NN nn)
{
    size_t i;
    for (i = 0; i < nn.arch_count - 1; i++) {
        mat_dot(row_as_mat(nn.as[i + 1]), row_as_mat(nn.as[i]), nn.ws[i]);
        mat_sum(row_as_mat(nn.as[i + 1]), row_as_mat(nn.bs[i]));
        mat_act(row_as_mat(nn.as[i + 1]));
    }
}

f32 nn_cost(NN nn, Mat t)
{
    size_t n, i, j, q;
    f32 c = 0.0f;

    ARENA_ASSERT(NN_INPUT(nn).cols + NN_OUTPUT(nn).cols == t.cols);
    n = t.rows;

    for (i = 0; i < n; i++) {
        Row row = mat_row(t, i);
        Row x = row_slice(row, 0, NN_INPUT(nn).cols);
        Row y = row_slice(row, NN_INPUT(nn).cols, NN_OUTPUT(nn).cols);

        row_copy(NN_INPUT(nn), x);
        nn_forward(nn);
        q = y.cols;
        for (j = 0; j < q; j++) {
            f32 d = ROW_AT(NN_OUTPUT(nn), j) - ROW_AT(y, j);
            c += d * d;
        }
    }

    return c / (f32)n;
}

NN nn_backprop(Arena *a, NN nn, Mat t)
{
    size_t n, i, j, k, l;
    f32 s;
    NN g;

    ARENA_ASSERT(NN_INPUT(nn).cols + NN_OUTPUT(nn).cols == t.cols);
    n = t.rows;

    g = nn_alloc(a, nn.arch, nn.arch_count);
    nn_zero(g);

    /* i - current sample, l - current layer, j - current activation, k - previous activation */
    for (i = 0; i < n; i++) {
        Row row = mat_row(t, i);
        Row in = row_slice(row, 0, NN_INPUT(nn).cols);
        Row out = row_slice(row, NN_INPUT(nn).cols, NN_OUTPUT(nn).cols);

        row_copy(NN_INPUT(nn), in);
        nn_forward(nn);

        for (j = 0; j < nn.arch_count; j++) row_fill(g.as[j], 0.0f);

#ifdef NN_BACKPROP_TRADITIONAL
        s = 1.0f;
        for (j = 0; j < out.cols; j++) {
            ROW_AT(NN_OUTPUT(g), j) = 2.0f * (ROW_AT(NN_OUTPUT(nn), j) - ROW_AT(out, j));
        }
#else
        s = 2.0f;
        for (j = 0; j < out.cols; j++) {
            ROW_AT(NN_OUTPUT(g), j) = ROW_AT(NN_OUTPUT(nn), j) - ROW_AT(out, j);
        }
#endif

        for (l = nn.arch_count - 1; l > 0; l--) {
            for (j = 0; j < nn.as[l].cols; j++) {
                f32 av = ROW_AT(nn.as[l], j);
                f32 da = ROW_AT(g.as[l], j);
                f32 qa = dactf_(av, NN_ACT);
                ROW_AT(g.bs[l - 1], j) += s * da * qa;
                for (k = 0; k < nn.as[l - 1].cols; k++) {
                    f32 pa = ROW_AT(nn.as[l - 1], k);
                    f32 w = MAT_AT(nn.ws[l - 1], k, j);
                    MAT_AT(g.ws[l - 1], k, j) += s * da * qa * pa;
                    ROW_AT(g.as[l - 1], k) += s * da * qa * w;
                }
            }
        }
    }

    for (i = 0; i < g.arch_count - 1; i++) {
        for (j = 0; j < g.ws[i].rows; j++) {
            for (k = 0; k < g.ws[i].cols; k++) {
                MAT_AT(g.ws[i], j, k) /= (f32)n;
            }
        }
        for (k = 0; k < g.bs[i].cols; k++) {
            ROW_AT(g.bs[i], k) /= (f32)n;
        }
    }

    return g;
}

NN nn_finite_diff(Arena *a, NN nn, Mat t, f32 eps)
{
    f32 saved, c;
    NN g;
    size_t i, j, k;

    c = nn_cost(nn, t);
    g = nn_alloc(a, nn.arch, nn.arch_count);

    for (i = 0; i < nn.arch_count - 1; i++) {
        for (j = 0; j < nn.ws[i].rows; j++) {
            for (k = 0; k < nn.ws[i].cols; k++) {
                saved = MAT_AT(nn.ws[i], j, k);
                MAT_AT(nn.ws[i], j, k) += eps;
                MAT_AT(g.ws[i], j, k) = (nn_cost(nn, t) - c) / eps;
                MAT_AT(nn.ws[i], j, k) = saved;
            }
        }
        for (k = 0; k < nn.bs[i].cols; k++) {
            saved = ROW_AT(nn.bs[i], k);
            ROW_AT(nn.bs[i], k) += eps;
            ROW_AT(g.bs[i], k) = (nn_cost(nn, t) - c) / eps;
            ROW_AT(nn.bs[i], k) = saved;
        }
    }

    return g;
}

void nn_learn(NN nn, NN g, f32 rate)
{
    size_t i, j, k;
    for (i = 0; i < nn.arch_count - 1; i++) {
        for (j = 0; j < nn.ws[i].rows; j++) {
            for (k = 0; k < nn.ws[i].cols; k++) {
                MAT_AT(nn.ws[i], j, k) -= rate * MAT_AT(g.ws[i], j, k);
            }
        }
        for (k = 0; k < nn.bs[i].cols; k++) {
            ROW_AT(nn.bs[i], k) -= rate * ROW_AT(g.bs[i], k);
        }
    }
}

void batch_process(Arena *a, Batch *b, size_t batch_size, NN nn, Mat t, f32 rate)
{
    size_t size;
    Mat batch_t;
    NN g;

    if (b->finished) {
        b->finished = 0;
        b->begin = 0;
        b->cost = 0.0f;
    }

    size = batch_size;
    if (b->begin + batch_size >= t.rows) {
        size = t.rows - b->begin;
    }

    batch_t.rows = size;
    batch_t.cols = t.cols;
    batch_t.elements = &MAT_AT(t, b->begin, 0);

    g = nn_backprop(a, nn, batch_t);
    nn_learn(nn, g, rate);
    b->cost += nn_cost(nn, batch_t);
    b->begin += batch_size;

    if (b->begin >= t.rows) {
        size_t batch_count = (t.rows + batch_size - 1) / batch_size;
        b->cost /= (f32)batch_count;
        b->finished = 1;
    }
}

#endif /* NN_IMPLEMENTATION_ */
#endif /* NN_IMPLEMENTATION */
