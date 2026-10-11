#include "test_common.h"
#define ARCHITECT_IMPLEMENTATION
#include "architect.h"

int main(void)
{
    Architect a;

    print_cstr("test_architect\n");

    architect_init(&a);
    TEST_CHECK(a.difficulty_scalar == 1.0f, "difficulty starts at baseline (1.0)");
    TEST_CHECK(a.death_rate_ema == 0.0f, "no deaths reported yet");

    /* a rough player: repeated deaths, high damage taken - frustration
     * should climb and difficulty should ease off */
    {
        i32 i;
        for (i = 0; i < 20; i++) {
            architect_report_death(&a, 1, 0.3f);
            architect_report_damage_taken(&a, 0.9f, 0.3f);
            architect_report_objective_result(&a, 0, 0.3f);
            architect_update(&a, 1.0f, 0.5f);
        }
        TEST_CHECK(a.frustration > 0.6f, "repeated deaths and heavy damage push frustration well above the flow target");
        TEST_CHECK(a.difficulty_scalar < 1.0f, "the director eases difficulty off for a struggling player");
    }

    /* an ace player: no deaths, no damage, always succeeds -
     * frustration should be low and difficulty should climb */
    {
        Architect b;
        i32 i;
        architect_init(&b);
        for (i = 0; i < 20; i++) {
            architect_report_death(&b, 0, 0.3f);
            architect_report_damage_taken(&b, 0.0f, 0.3f);
            architect_report_objective_result(&b, 1, 0.3f);
            architect_update(&b, 1.0f, 0.5f);
        }
        TEST_CHECK(b.frustration < 0.2f, "a flawless player's frustration stays low");
        TEST_CHECK(b.difficulty_scalar > 1.0f, "the director ramps difficulty UP for a player who's clearly not being challenged");
    }

    /* engagement peaks somewhere in the MIDDLE of the frustration
     * range (the flow channel), not at either extreme - a bored
     * flawless player and an overwhelmed struggling player should
     * both engage LESS than a player facing a real, balanced mix of
     * wins and setbacks */
    {
        Architect bored, balanced, overwhelmed;

        architect_init(&bored);
        architect_report_death(&bored, 0, 1.0f);
        architect_report_damage_taken(&bored, 0.0f, 1.0f);
        architect_report_objective_result(&bored, 1, 1.0f);
        architect_update(&bored, 1.0f, 0.0f); /* adjust_rate 0: only recompute frustration/engagement, don't move difficulty */

        architect_init(&balanced);
        architect_report_death(&balanced, 1, 1.0f);
        architect_report_damage_taken(&balanced, 0.3f, 1.0f);
        architect_report_objective_result(&balanced, 1, 1.0f);
        architect_update(&balanced, 1.0f, 0.0f);

        architect_init(&overwhelmed);
        architect_report_death(&overwhelmed, 1, 1.0f);
        architect_report_damage_taken(&overwhelmed, 1.0f, 1.0f);
        architect_report_objective_result(&overwhelmed, 0, 1.0f);
        architect_update(&overwhelmed, 1.0f, 0.0f);

        TEST_CHECK(bored.frustration < balanced.frustration && balanced.frustration < overwhelmed.frustration,
                   "test setup sanity check: frustration really does increase bored -> balanced -> overwhelmed");
        TEST_CHECK(balanced.engagement > bored.engagement, "a balanced mix of wins/setbacks engages more than a flawless, bored run");
        TEST_CHECK(balanced.engagement > overwhelmed.engagement, "a balanced mix engages more than being completely overwhelmed too");
    }

    /* repeated-event exploit detection */
    {
        Architect e;
        i32 flagged = 0;
        i32 i;
        architect_init(&e);

        for (i = 0; i < 10 && !flagged; i++) {
            flagged = architect_report_event(&e, 42, 5.0f, 5);
        }
        TEST_CHECK(flagged, "farming the same event id repeatedly within the anomaly window eventually flags");
        TEST_CHECK(e.event_repeat_count >= 5, "the repeat counter actually reached the configured threshold");
    }
    {
        Architect e;
        i32 i;
        i32 flagged = 0;
        architect_init(&e);
        for (i = 0; i < 4; i++) flagged |= architect_report_event(&e, 1, 5.0f, 5);
        TEST_CHECK(!flagged, "4 repeats under a threshold of 5 does not flag yet");

        flagged = architect_report_event(&e, 2, 5.0f, 5); /* a DIFFERENT event id resets the streak */
        TEST_CHECK(!flagged, "switching to a different event id resets the streak, does not carry the count over");
    }

    /* reaction-time anomaly: suspiciously consistent (scripted-looking)
     * timings vs naturally varying ones */
    {
        Architect r;
        i32 i;
        architect_init(&r);
        for (i = 0; i < 20; i++) architect_report_reaction_time(&r, 0.250f); /* identical every time - zero variance */
        TEST_CHECK(architect_reaction_time_is_anomalous(&r, 0.001f), "perfectly identical reaction times (zero variance) are flagged as anomalous");
    }
    {
        Architect r;
        f32 samples[10];
        i32 i;
        samples[0]=0.21f; samples[1]=0.34f; samples[2]=0.19f; samples[3]=0.29f; samples[4]=0.41f;
        samples[5]=0.22f; samples[6]=0.31f; samples[7]=0.18f; samples[8]=0.27f; samples[9]=0.35f;
        architect_init(&r);
        for (i = 0; i < 10; i++) architect_report_reaction_time(&r, samples[i]);
        TEST_CHECK(!architect_reaction_time_is_anomalous(&r, 0.001f), "naturally varying reaction times are not flagged");
    }
    {
        Architect r;
        architect_init(&r);
        architect_report_reaction_time(&r, 0.25f);
        TEST_CHECK(!architect_reaction_time_is_anomalous(&r, 0.001f), "too few samples (< 8) never flags, not enough data to trust yet");
    }

    print_flush();
    return test_failures_ != 0;
}
