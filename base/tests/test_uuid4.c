#include "test_common.h"
#define RNG_IMPLEMENTATION
#define UUID_IMPLEMENTATION
#include "rng.h"
#include "uuid4.h"

#define N 2000

static i32 is_hex_digit_(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

int main(void)
{
    Rng rng;
    Uuid4 id;
    char str[UUID4_STR_LEN];
    size_t i, j;
    Uuid4 ids[N];
    i32 all_valid_version, all_valid_variant, no_dups;

    print_cstr("test_uuid\n");

    pcg32_seed(&rng, 42, 1);
    id = uuid4_generate(&rng);

    TEST_CHECK((id.bytes[6] >> 4) == 4, "version nibble is 4");
    TEST_CHECK((id.bytes[8] >> 6) == 2, "variant bits are 10");

    uuid4_to_string(id, str);
    TEST_CHECK(str[36] == '\0', "string is NUL-terminated at index 36");
    TEST_CHECK(str[8] == '-' && str[13] == '-' && str[18] == '-' && str[23] == '-',
               "dashes land at the 8-4-4-4-12 boundaries");

    {
        i32 all_hex = 1;
        for (i = 0; i < 36; i++) {
            if (i == 8 || i == 13 || i == 18 || i == 23) continue;
            if (!is_hex_digit_(str[i])) { all_hex = 0; break; }
        }
        TEST_CHECK(all_hex, "every non-dash character is a lowercase hex digit");
    }

    for (i = 0; i < N; i++) ids[i] = uuid4_generate(&rng);

    all_valid_version = 1;
    all_valid_variant = 1;
    for (i = 0; i < N; i++) {
        if ((ids[i].bytes[6] >> 4) != 4) all_valid_version = 0;
        if ((ids[i].bytes[8] >> 6) != 2) all_valid_variant = 0;
    }
    TEST_CHECK(all_valid_version, "version nibble is 4 across many generated ids");
    TEST_CHECK(all_valid_variant, "variant bits are 10 across many generated ids");

    no_dups = 1;
    for (i = 0; i < N && no_dups; i++) {
        for (j = i + 1; j < N; j++) {
            i32 eq = 1, k;
            for (k = 0; k < 16; k++) {
                if (ids[i].bytes[k] != ids[j].bytes[k]) { eq = 0; break; }
            }
            if (eq) { no_dups = 0; break; }
        }
    }
    TEST_CHECK(no_dups, "no duplicate ids among 2000 generated from one seeded stream");

    {
        Rng os_rng;
        i32 seeded = uuid4_seed_from_os(&os_rng);
        TEST_CHECK(seeded == 0, "uuid4_seed_from_os succeeds on this system (/dev/urandom readable)");
        if (seeded == 0) {
            Uuid4 os_id = uuid4_generate(&os_rng);
            TEST_CHECK((os_id.bytes[6] >> 4) == 4, "OS-seeded id also has version nibble 4");
        }
    }

    print_flush();
    return test_failures_ != 0;
}
