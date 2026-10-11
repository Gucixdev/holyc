#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define NN_IMPLEMENTATION
#include "arena_alloc.h"
#include "nn.h"

/* the canonical smoke test for this exact design (tsoding's own demos/xor.c
 * does the same thing) - 2 inputs, one hidden layer, 1 output, XOR is not
 * linearly separable so a network that gets this right after training is
 * actually exercising forward+backprop+learn correctly, not just
 * memorizing a linear boundary */
static f32 xor_data_[] = {
    0, 0, 0,
    0, 1, 1,
    1, 0, 1,
    1, 1, 0
};

int main(void)
{
    Arena a = {0};
    Nn_Rng rng;
    size_t arch[] = { 2, 2, 1 };
    NN nn, g_backprop, g_finite;
    Mat t;
    size_t i, j;
    i32 grads_agree;
    i32 xor_correct;
    f32 cost_before, cost_after;

    print_cstr("test_nn\n");

    nn_rng_seed(&rng, 12345);

    t.rows = 4;
    t.cols = 3;
    t.elements = xor_data_;

    nn = nn_alloc(&a, arch, 3);
    nn_rand(&rng, nn, -1.0f, 1.0f);

    /* finite-difference gradient must agree (within a loose epsilon - it's
     * a numerical approximation) with analytic backprop, for every weight
     * and bias - this is the library's own built-in correctness check for
     * whether the backprop derivation is right at all, not a property
     * specific to XOR */
    g_backprop = nn_backprop(&a, nn, t);
    g_finite = nn_finite_diff(&a, nn, t, 1e-1f);

    grads_agree = 1;
    for (i = 0; i < nn.arch_count - 1; i++) {
        for (j = 0; j < g_backprop.ws[i].rows * g_backprop.ws[i].cols; j++) {
            f32 diff = g_backprop.ws[i].elements[j] - g_finite.ws[i].elements[j];
            if (diff < 0.0f) diff = -diff;
            if (diff > 0.1f) { grads_agree = 0; }
        }
    }
    TEST_CHECK(grads_agree, "backprop gradient agrees with finite-difference gradient (within epsilon)");

    /* train for real: cost should drop substantially and the network
     * should end up correctly predicting XOR on all 4 inputs */
    cost_before = nn_cost(nn, t);

    for (i = 0; i < 20000; i++) {
        NN g = nn_backprop(&a, nn, t);
        nn_learn(nn, g, 1.0f);
    }

    cost_after = nn_cost(nn, t);
    TEST_CHECK(cost_after < cost_before * 0.01f, "training drops cost by at least 100x over 20000 steps");

    xor_correct = 1;
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            f32 expected = (f32)((i32)i ^ (i32)j);
            f32 got;
            ROW_AT(NN_INPUT(nn), 0) = (f32)i;
            ROW_AT(NN_INPUT(nn), 1) = (f32)j;
            nn_forward(nn);
            got = ROW_AT(NN_OUTPUT(nn), 0);
            if ((got > 0.5f ? 1.0f : 0.0f) != expected) xor_correct = 0;
        }
    }
    TEST_CHECK(xor_correct, "trained network correctly predicts XOR for all 4 input combinations");

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
