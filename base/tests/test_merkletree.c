#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define MERKLETREE_IMPLEMENTATION
#include "merkletree.h"

int main(void)
{
    const char *blocks_str[5];
    const void *blocks[5];
    size_t sizes[5];
    size_t i;
    Merkle_Hash root, root2;

    print_cstr("test_merkletree\n");

    blocks_str[0] = "alpha";
    blocks_str[1] = "bravo";
    blocks_str[2] = "charlie";
    blocks_str[3] = "delta";
    blocks_str[4] = "echo"; /* odd count - exercises the "odd node promoted" path */

    for (i = 0; i < 5; i++) {
        blocks[i] = blocks_str[i];
        sizes[i] = 0;
        { const char *s = blocks_str[i]; while (s[sizes[i]]) sizes[i]++; }
    }

    root = merkletree_root(blocks, sizes, 5, NULL);
    root2 = merkletree_root(blocks, sizes, 5, NULL);
    {
        i32 same = 1;
        for (i = 0; i < SHA256_DIGEST_SIZE; i++) if (root.hash[i] != root2.hash[i]) same = 0;
        TEST_CHECK(same, "the root hash is deterministic across repeated calls on the same data");
    }

    {
        const char *different[5];
        const void *db[5];
        size_t ds[5];
        Merkle_Hash root3;
        i32 j;
        for (j = 0; j < 5; j++) different[j] = blocks_str[j];
        different[4] = "ECHO"; /* one byte changed in the last block */
        for (j = 0; j < 5; j++) {
            db[j] = different[j];
            ds[j] = 0;
            { const char *s = different[j]; while (s[ds[j]]) ds[j]++; }
        }
        root3 = merkletree_root(db, ds, 5, NULL);
        {
            i32 same = 1;
            for (i = 0; i < SHA256_DIGEST_SIZE; i++) if (root.hash[i] != root3.hash[i]) same = 0;
            TEST_CHECK(!same, "changing even one block changes the root hash entirely");
        }
    }

    /* proof + verify for every leaf index, including the odd-one-out
     * last leaf */
    for (i = 0; i < 5; i++) {
        Merkle_Hash proof[16];
        size_t proof_len = merkletree_proof(blocks, sizes, 5, i, proof, 16);
        i32 ok = merkletree_verify(blocks[i], sizes[i], i, 5, proof, proof_len, root);
        TEST_CHECK(ok, "a valid proof for each real leaf index verifies successfully against the root");
    }

    /* a proof verified against the WRONG leaf data must fail */
    {
        Merkle_Hash proof[16];
        size_t proof_len = merkletree_proof(blocks, sizes, 5, 0, proof, 16);
        i32 ok = merkletree_verify("wrong data", 10, 0, 5, proof, proof_len, root);
        TEST_CHECK(!ok, "verifying with tampered leaf data against a real proof fails");
    }

    /* a proof verified against the WRONG index must fail (even with
     * otherwise-correct leaf data and proof, mismatched index breaks
     * the sibling left/right ordering) */
    {
        Merkle_Hash proof[16];
        size_t proof_len = merkletree_proof(blocks, sizes, 5, 1, proof, 16);
        i32 ok = merkletree_verify(blocks[1], sizes[1], 0, 5, proof, proof_len, root); /* claims index 0, proof was built for index 1 */
        TEST_CHECK(!ok, "verifying with a mismatched index fails");
    }

    print_flush();
    return test_failures_ != 0;
}
