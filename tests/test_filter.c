/* Filter: exact integer expectations. Every value below is computed by hand
 * from the documented rounding rule, not copied from a run of the code. */
#include "ef_test.h"
#include "filter.h"

EF_TEST(init_validates_configuration)
{
    ef_filter f;
    EF_ASSERT(ef_filter_init(&f, 1u, 0, 1u));
    EF_ASSERT(ef_filter_init(&f, 64u, 0, 1u));
    EF_ASSERT(!ef_filter_init(&f, 0u, 0, 1u));
    EF_ASSERT(!ef_filter_init(&f, 3u, 0, 1u));    /* not a power of two   */
    EF_ASSERT(!ef_filter_init(&f, 128u, 0, 1u));  /* over MAX_WINDOW      */
    EF_ASSERT(!ef_filter_init(&f, 4u, 0, 0u));    /* debounce of 0        */
}

EF_TEST(starts_empty_and_low)
{
    ef_filter f;
    EF_ASSERT(ef_filter_init(&f, 4u, 100, 2u));
    EF_ASSERT_EQ_I(ef_filter_value(&f), 0);
    EF_ASSERT_EQ_I(ef_filter_state_get(&f), EF_FILTER_LOW);
    EF_ASSERT_EQ_I(f.samples, 0);
}

/* Before the window fills, the average is over the samples actually seen. A
 * naive "sum >> shift" would report 25 after a single sample of 100, which
 * makes the first second of every power-up a lie. */
EF_TEST(partial_window_divides_by_samples_seen)
{
    ef_filter f;
    EF_ASSERT(ef_filter_init(&f, 4u, 1000000, 1u));
    (void)ef_filter_push(&f, 100);
    EF_ASSERT_EQ_I(ef_filter_value(&f), 100);       /* 100/1 */
    (void)ef_filter_push(&f, 200);
    EF_ASSERT_EQ_I(ef_filter_value(&f), 150);       /* 300/2 */
    (void)ef_filter_push(&f, 300);
    EF_ASSERT_EQ_I(ef_filter_value(&f), 200);       /* 600/3 */
    (void)ef_filter_push(&f, 400);
    EF_ASSERT_EQ_I(ef_filter_value(&f), 250);       /* 1000/4, window now full */
}

EF_TEST(moving_average_slides)
{
    ef_filter f;
    EF_ASSERT(ef_filter_init(&f, 4u, 1000000, 1u));
    (void)ef_filter_push(&f, 100);
    (void)ef_filter_push(&f, 200);
    (void)ef_filter_push(&f, 300);
    (void)ef_filter_push(&f, 400);
    (void)ef_filter_push(&f, 500);                  /* drops the 100 */
    EF_ASSERT_EQ_I(ef_filter_value(&f), 350);       /* 1400/4 */
    (void)ef_filter_push(&f, 600);
    EF_ASSERT_EQ_I(ef_filter_value(&f), 450);       /* 1800/4 */
    /* A long run of one value converges exactly onto it. */
    {
        int i;
        for (i = 0; i < 8; i++) { (void)ef_filter_push(&f, 42); }
        EF_ASSERT_EQ_I(ef_filter_value(&f), 42);
    }
}

/* Rounding is half-away-from-zero and therefore symmetric. An arithmetic-shift
 * implementation would floor, giving 2 and -3 for the pair below. */
EF_TEST(rounding_is_symmetric_about_zero)
{
    ef_filter f;
    EF_ASSERT(ef_filter_init(&f, 4u, 1000000, 1u));
    (void)ef_filter_push(&f, 4);
    (void)ef_filter_push(&f, 3);
    (void)ef_filter_push(&f, 2);
    (void)ef_filter_push(&f, 1);
    EF_ASSERT_EQ_I(ef_filter_value(&f), 3);         /* 10/4 = 2.5 -> 3 */

    EF_ASSERT(ef_filter_init(&f, 4u, 1000000, 1u));
    (void)ef_filter_push(&f, -4);
    (void)ef_filter_push(&f, -3);
    (void)ef_filter_push(&f, -2);
    (void)ef_filter_push(&f, -1);
    EF_ASSERT_EQ_I(ef_filter_value(&f), -3);        /* -10/4 = -2.5 -> -3 */
}

EF_TEST(window_of_one_is_passthrough)
{
    ef_filter f;
    EF_ASSERT(ef_filter_init(&f, 1u, 1000000, 1u));
    (void)ef_filter_push(&f, 12345);
    EF_ASSERT_EQ_I(ef_filter_value(&f), 12345);
    (void)ef_filter_push(&f, -99);
    EF_ASSERT_EQ_I(ef_filter_value(&f), -99);
}

/* Debounce: N consecutive agreeing samples are required, and the transition
 * lands on exactly the Nth. */
EF_TEST(debounce_requires_n_consecutive)
{
    ef_filter f;
    EF_ASSERT(ef_filter_init(&f, 1u, 100, 3u));

    EF_ASSERT_EQ_I(ef_filter_push(&f, 150), EF_FILTER_LOW);  /* 1 of 3 */
    EF_ASSERT_EQ_I(ef_filter_push(&f, 150), EF_FILTER_LOW);  /* 2 of 3 */
    EF_ASSERT_EQ_I(ef_filter_push(&f, 150), EF_FILTER_HIGH); /* 3 of 3 */
    EF_ASSERT_EQ_I(f.transitions, 1);

    EF_ASSERT_EQ_I(ef_filter_push(&f, 50), EF_FILTER_HIGH);
    EF_ASSERT_EQ_I(ef_filter_push(&f, 50), EF_FILTER_HIGH);
    EF_ASSERT_EQ_I(ef_filter_push(&f, 50), EF_FILTER_LOW);
    EF_ASSERT_EQ_I(f.transitions, 2);
}

EF_TEST(debounce_of_one_switches_immediately)
{
    ef_filter f;
    EF_ASSERT(ef_filter_init(&f, 1u, 100, 1u));
    EF_ASSERT_EQ_I(ef_filter_push(&f, 100), EF_FILTER_HIGH); /* >= threshold */
    EF_ASSERT_EQ_I(ef_filter_push(&f, 99), EF_FILTER_LOW);
    EF_ASSERT_EQ_I(f.transitions, 2);
}

/* The reason debounce exists: a signal dithering across the threshold must
 * produce zero transitions, not one per sample. */
EF_TEST(dithering_signal_never_transitions)
{
    ef_filter f;
    int i;
    EF_ASSERT(ef_filter_init(&f, 1u, 100, 3u));
    for (i = 0; i < 500; i++) {
        (void)ef_filter_push(&f, (i % 2 == 0) ? 101 : 99);
    }
    EF_ASSERT_EQ_I(f.transitions, 0);
    EF_ASSERT_EQ_I(ef_filter_state_get(&f), EF_FILTER_LOW);
}

/* A run interrupted one sample short of the requirement restarts from 1. */
EF_TEST(interrupted_run_restarts_the_count)
{
    ef_filter f;
    EF_ASSERT(ef_filter_init(&f, 1u, 100, 3u));
    (void)ef_filter_push(&f, 150);
    (void)ef_filter_push(&f, 150);   /* 2 of 3 */
    (void)ef_filter_push(&f, 10);    /* breaks the run */
    (void)ef_filter_push(&f, 150);
    (void)ef_filter_push(&f, 150);
    EF_ASSERT_EQ_I(ef_filter_state_get(&f), EF_FILTER_LOW); /* only 2 again */
    EF_ASSERT_EQ_I(ef_filter_push(&f, 150), EF_FILTER_HIGH);
}

EF_TEST(threshold_is_inclusive)
{
    ef_filter f;
    EF_ASSERT(ef_filter_init(&f, 1u, 100, 1u));
    EF_ASSERT_EQ_I(ef_filter_push(&f, 99), EF_FILTER_LOW);
    EF_ASSERT_EQ_I(ef_filter_push(&f, 100), EF_FILTER_HIGH);
}

EF_TEST(set_threshold_at_runtime)
{
    ef_filter f;
    EF_ASSERT(ef_filter_init(&f, 1u, 1000, 2u));
    (void)ef_filter_push(&f, 500);
    (void)ef_filter_push(&f, 500);
    EF_ASSERT_EQ_I(ef_filter_state_get(&f), EF_FILTER_LOW);

    ef_filter_set_threshold(&f, 400);
    EF_ASSERT_EQ_I(ef_filter_threshold(&f), 400);
    /* The window is not cleared, but the debounce run restarts: two more
     * samples are needed, the pending count does not carry over. */
    EF_ASSERT_EQ_I(ef_filter_push(&f, 500), EF_FILTER_LOW);
    EF_ASSERT_EQ_I(ef_filter_push(&f, 500), EF_FILTER_HIGH);
}

/* The accumulator cannot overflow because the input is clamped first. */
EF_TEST(samples_are_clamped)
{
    ef_filter f;
    EF_ASSERT(ef_filter_init(&f, 1u, 0, 1u));
    (void)ef_filter_push(&f, 2000000000);
    EF_ASSERT_EQ_I(ef_filter_value(&f), EF_FILTER_SAMPLE_MAX);
    (void)ef_filter_push(&f, -2000000000);
    EF_ASSERT_EQ_I(ef_filter_value(&f), -EF_FILTER_SAMPLE_MAX);
    EF_ASSERT_EQ_I(f.clamped, 2);
    /* In range: untouched. */
    (void)ef_filter_push(&f, 1000);
    EF_ASSERT_EQ_I(f.clamped, 2);
}

/* Fill the largest window with the largest permitted samples of alternating
 * sign, thousands of times, and confirm the running sum stays in the range the
 * header claims. If the clamp or the bound were wrong this would blow up. */
EF_TEST(accumulator_stays_in_range_under_worst_case)
{
    ef_filter f;
    ef_rng rng = { 0xBEEF01u };
    int i;
    const int32_t bound = (int32_t)EF_FILTER_MAX_WINDOW * (int32_t)EF_FILTER_SAMPLE_MAX;

    EF_ASSERT(ef_filter_init(&f, (uint8_t)EF_FILTER_MAX_WINDOW, 0, 1u));
    for (i = 0; i < 50000; i++) {
        int32_t s = (ef_rng_next(&rng) & 1u) ? 2000000000 : -2000000000;
        (void)ef_filter_push(&f, s);
        EF_ASSERT(f.sum <= bound && f.sum >= -bound);
        EF_ASSERT(ef_filter_value(&f) <= EF_FILTER_SAMPLE_MAX);
        EF_ASSERT(ef_filter_value(&f) >= -EF_FILTER_SAMPLE_MAX);
    }
}

/* The average is never outside the range of the samples in the window -- the
 * defining property of a mean, and a cheap invariant to check against random
 * input. */
EF_TEST(average_is_bounded_by_window_extremes)
{
    ef_rng rng = { 0x5EEDu };
    int trial;

    for (trial = 0; trial < 200; trial++) {
        ef_filter f;
        int32_t recent[8];
        int n = 0, i;
        EF_ASSERT(ef_filter_init(&f, 8u, 0, 1u));

        for (i = 0; i < 300; i++) {
            int32_t s = (int32_t)(ef_rng_next(&rng) % 200001u) - 100000;
            int32_t lo, hi, avg;
            int k;

            recent[i % 8] = s;
            if (n < 8) { n++; }
            (void)ef_filter_push(&f, s);

            lo = recent[0];
            hi = recent[0];
            for (k = 0; k < n; k++) {
                if (recent[k] < lo) { lo = recent[k]; }
                if (recent[k] > hi) { hi = recent[k]; }
            }
            avg = ef_filter_value(&f);
            EF_ASSERT(avg >= lo - 1 && avg <= hi + 1); /* +-1 for rounding */
        }
    }
}

int main(void)
{
    EF_RUN(init_validates_configuration);
    EF_RUN(starts_empty_and_low);
    EF_RUN(partial_window_divides_by_samples_seen);
    EF_RUN(moving_average_slides);
    EF_RUN(rounding_is_symmetric_about_zero);
    EF_RUN(window_of_one_is_passthrough);
    EF_RUN(debounce_requires_n_consecutive);
    EF_RUN(debounce_of_one_switches_immediately);
    EF_RUN(dithering_signal_never_transitions);
    EF_RUN(interrupted_run_restarts_the_count);
    EF_RUN(threshold_is_inclusive);
    EF_RUN(set_threshold_at_runtime);
    EF_RUN(samples_are_clamped);
    EF_RUN(accumulator_stays_in_range_under_worst_case);
    EF_RUN(average_is_bounded_by_window_extremes);
    return ef_test_report("filter");
}
