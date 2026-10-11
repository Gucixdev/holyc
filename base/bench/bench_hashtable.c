#define PRINT_IMPLEMENTATION
#define BASE_TYPE_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define HASHTABLE_IMPLEMENTATION
#include "util/print.h"
#include "alloc/arena_alloc.h"
#include "data/hash/hashtable.h"
#include "bench_util.h"
#include <stdlib.h>

/* naive = look a key up by scanning a plain array of {key, value} pairs
 * one at a time until a match is found - the thing you'd actually write
 * without a hash table at all. This is a real, fair "what if we didn't
 * hash" baseline, not an invented one. */

typedef struct { char key[16]; size_t key_len; i32 value; } Naive_Pair_;

static i32 naive_get_(const Naive_Pair_ *pairs, size_t count, const char *key, size_t key_len)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (pairs[i].key_len == key_len && memcmp(pairs[i].key, key, key_len) == 0) {
            return pairs[i].value;
        }
    }
    return -1;
}

int main(void)
{
    i64 sizes[] = { 16, 256, 4096 };
    size_t si;

    print_cstr("bench_hashtable: put N keys, then look up every key once\n");
    print_cstr("naive = linear scan over a plain {key,value} array\n");
    print_cstr("ht    = open-addressing hash table (hashtable.h)\n\n");

    for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        size_t n = (size_t)sizes[si];
        Naive_Pair_ *pairs = (Naive_Pair_ *)malloc(n * sizeof(Naive_Pair_));
        char (*keys)[16] = (char (*)[16])malloc(n * 16);
        size_t i;
        i64 iters = (n <= 256) ? 200 : 20;
        f64 t_naive, t_ht;
        i64 sink = 0;

        for (i = 0; i < n; i++) {
            int len = 4 + (int)(i % 8);
            int c;
            for (c = 0; c < len; c++) keys[i][c] = (char)('a' + ((i + (size_t)c) % 26));
            pairs[i].key_len = (size_t)len;
            memcpy(pairs[i].key, keys[i], (size_t)len);
            pairs[i].value = (i32)i;
        }

        BENCH_TIMEIT(t_naive, iters, {
            for (i = 0; i < n; i++) {
                sink += naive_get_(pairs, n, keys[i], pairs[i].key_len);
            }
        });

        BENCH_TIMEIT(t_ht, iters, {
            Arena a = {0};
            Ht ht;
            ht_init(&a, &ht, n);
            for (i = 0; i < n; i++) ht_put(&a, &ht, keys[i], pairs[i].key_len, &pairs[i].value);
            for (i = 0; i < n; i++) {
                i32 *v = (i32 *)ht_get(&ht, keys[i], pairs[i].key_len);
                sink += *v;
            }
            arena_free(&a);
        });

        print_cstr("n="); print_u64((u64)n);
        print_cstr("  naive="); print_u64((u64)(t_naive * 1e6)); print_cstr(" us");
        print_cstr("  ht="); print_u64((u64)(t_ht * 1e6)); print_cstr(" us");
        print_cstr("  speedup="); print_u64(bench_speedup_pct(t_naive, t_ht)); print_cstr("% (100 = same)\n");

        free(pairs);
        free(keys);
        if (sink == -12345) print_cstr(""); /* keep sink from being optimized away entirely */
    }

    print_flush();
    return 0;
}
