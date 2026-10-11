#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define ARRAY_IMPLEMENTATION
#define HASHTABLE_IMPLEMENTATION
#define HASH_MAP_IMPLEMENTATION
#define MERGE_OPS_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "data/array.h"
#include "data/hash/hashmap.h"
#include "data/merge_ops.h"

#include <string.h>

static i32 cmp_i32_(const void *x, const void *y)
{
    i32 a = *(const i32 *)x, b = *(const i32 *)y;
    return (a > b) - (a < b);
}

int main(void)
{
    Arena arena = {0};

    print_cstr("test_merge_ops\n");

    {
        Array a, b, out;
        i32 vals_a[] = { 1, 3, 5, 7 };
        i32 vals_b[] = { 2, 4, 6 };
        i32 i;

        array_static_init(&a, vals_a, 4, sizeof(i32));
        a.count = 4;
        array_static_init(&b, vals_b, 3, sizeof(i32));
        b.count = 3;
        array_dynamic_init(&out, sizeof(i32));

        merge_ops_sorted_merge(&arena, &a, &b, cmp_i32_, &out);
        TEST_CHECK(out.count == 7, "merging a 4-element and a 3-element sorted array gives 7 elements");
        {
            i32 expected[7] = { 1, 2, 3, 4, 5, 6, 7 };
            i32 ok = 1;
            for (i = 0; i < 7; i++) {
                if (*(i32 *)array_get(&out, (size_t)i) != expected[i]) { ok = 0; break; }
            }
            TEST_CHECK(ok, "the merged result is fully sorted ascending, interleaving both inputs correctly");
        }
    }

    {
        Array a, out;
        i32 vals_a[] = { 1, 2, 3 };
        i32 empty_b[1];
        Array b;

        array_static_init(&a, vals_a, 3, sizeof(i32));
        a.count = 3;
        array_static_init(&b, empty_b, 0, sizeof(i32));
        b.count = 0;
        array_dynamic_init(&out, sizeof(i32));

        merge_ops_sorted_merge(&arena, &a, &b, cmp_i32_, &out);
        TEST_CHECK(out.count == 3, "merging against an empty array just copies the non-empty side through");
    }

    {
        Hash_Map dst, src;
        i32 v_shared_dst = 100, v_shared_src = 200, v_only_src = 300;

        hash_map_init(&arena, &dst, 8);
        hash_map_init(&arena, &src, 8);

        hash_map_put(&arena, &dst, "shared", 6, &v_shared_dst);
        hash_map_put(&arena, &src, "shared", 6, &v_shared_src);
        hash_map_put(&arena, &src, "only_in_src", 11, &v_only_src);

        merge_ops_hashmap_merge(&arena, &dst, &src, 0);
        TEST_CHECK(*(i32 *)hash_map_get(&dst, "shared", 6) == 100, "overwrite_existing=0: dst's own value for a shared key wins, src's is discarded");
        TEST_CHECK(*(i32 *)hash_map_get(&dst, "only_in_src", 11) == 300, "a brand-new key from src is still copied over even when overwrite_existing=0");
    }

    {
        Hash_Map dst, src;
        i32 v_shared_dst = 100, v_shared_src = 200;

        hash_map_init(&arena, &dst, 8);
        hash_map_init(&arena, &src, 8);

        hash_map_put(&arena, &dst, "shared", 6, &v_shared_dst);
        hash_map_put(&arena, &src, "shared", 6, &v_shared_src);

        merge_ops_hashmap_merge(&arena, &dst, &src, 1);
        TEST_CHECK(*(i32 *)hash_map_get(&dst, "shared", 6) == 200, "overwrite_existing=1: src's value for a shared key wins over dst's");
    }

    arena_free(&arena);
    print_flush();
    return test_failures_ != 0;
}
