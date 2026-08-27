/* Scheduler: periods, ordering, overrun accounting, and the tick wrap. */
#include "ef_test.h"
#include "sched.h"

typedef struct { uint32_t calls; uint32_t last_seen; } counter;

static void bump(void *ctx)
{
    ((counter *)ctx)->calls++;
}

EF_TEST(add_validates)
{
    ef_sched s;
    counter c = { 0u, 0u };
    unsigned i;

    ef_sched_init(&s);
    EF_ASSERT_EQ_I(ef_sched_add(&s, "null", NULL, &c, 1u, 0u), -1);

    for (i = 0u; i < EF_SCHED_MAX_TASKS; i++) {
        EF_ASSERT_EQ_I(ef_sched_add(&s, "t", bump, &c, 1u, 0u), (int)i);
    }
    /* Table full: refuses rather than writing past the array. */
    EF_ASSERT_EQ_I(ef_sched_add(&s, "overflow", bump, &c, 1u, 0u), -1);
    EF_ASSERT_EQ_I(s.count, EF_SCHED_MAX_TASKS);
}

EF_TEST(nothing_runs_before_its_deadline)
{
    ef_sched s;
    counter c = { 0u, 0u };
    ef_sched_init(&s);
    EF_ASSERT(ef_sched_add(&s, "t", bump, &c, 10u, 100u) >= 0);

    EF_ASSERT_EQ_I(ef_sched_run(&s, 0u), 0);
    EF_ASSERT_EQ_I(ef_sched_run(&s, 99u), 0);
    EF_ASSERT_EQ_I(c.calls, 0);
    EF_ASSERT_EQ_I(ef_sched_run(&s, 100u), 1);
    EF_ASSERT_EQ_I(c.calls, 1);
}

EF_TEST(period_is_honoured)
{
    ef_sched s;
    counter c = { 0u, 0u };
    uint32_t t;
    ef_sched_init(&s);
    EF_ASSERT(ef_sched_add(&s, "t", bump, &c, 10u, 0u) >= 0);

    /* One tick at a time for 1000 ticks: a 10-tick task runs 101 times
     * (t = 0, 10, 20, ... 1000). */
    for (t = 0u; t <= 1000u; t++) {
        (void)ef_sched_run(&s, t);
    }
    EF_ASSERT_EQ_I(c.calls, 101);
    EF_ASSERT_EQ_I(ef_sched_overruns(&s), 0);
}

EF_TEST(period_zero_runs_every_call)
{
    ef_sched s;
    counter c = { 0u, 0u };
    uint32_t t;
    ef_sched_init(&s);
    EF_ASSERT(ef_sched_add(&s, "t", bump, &c, 0u, 0u) >= 0);
    for (t = 0u; t < 50u; t++) {
        EF_ASSERT_EQ_I(ef_sched_run(&s, t), 1);
    }
    EF_ASSERT_EQ_I(c.calls, 50);
    /* Running every call is not an overrun. */
    EF_ASSERT_EQ_I(ef_sched_overruns(&s), 0);
}

EF_TEST(all_due_tasks_run_each_call)
{
    ef_sched s;
    counter c1 = { 0u, 0u }, c2 = { 0u, 0u }, c3 = { 0u, 0u };

    ef_sched_init(&s);
    /* Indices are handed out in insertion order, and ef_sched_run walks the
     * table in that order -- so task priority is simply add order. */
    EF_ASSERT_EQ_I(ef_sched_add(&s, "a", bump, &c1, 1u, 0u), 0);
    EF_ASSERT_EQ_I(ef_sched_add(&s, "b", bump, &c2, 2u, 0u), 1);
    EF_ASSERT_EQ_I(ef_sched_add(&s, "c", bump, &c3, 4u, 0u), 2);

    EF_ASSERT_EQ_I(ef_sched_run(&s, 0u), 3);   /* all three due at 0 */
    EF_ASSERT_EQ_I(ef_sched_run(&s, 1u), 1);   /* only a             */
    EF_ASSERT_EQ_I(ef_sched_run(&s, 2u), 2);   /* a and b            */
    EF_ASSERT_EQ_I(ef_sched_run(&s, 4u), 3);   /* all three          */

    EF_ASSERT_EQ_I(c1.calls, 4);
    EF_ASSERT_EQ_I(c2.calls, 3);
    EF_ASSERT_EQ_I(c3.calls, 2);

    EF_ASSERT_STR_EQ(ef_sched_task(&s, 0)->name, "a");
    EF_ASSERT_STR_EQ(ef_sched_task(&s, 2)->name, "c");
    EF_ASSERT(ef_sched_task(&s, 3) == NULL);   /* out of range, not a crash */
    EF_ASSERT_EQ_I(ef_sched_task(&s, 0)->runs, 4);
}

/* Missed deadlines are counted and abandoned, never replayed. A scheduler that
 * caught up would run this task 10 times in one call and starve everything
 * behind it. */
EF_TEST(overrun_is_counted_not_replayed)
{
    ef_sched s;
    counter c = { 0u, 0u };
    ef_sched_init(&s);
    EF_ASSERT(ef_sched_add(&s, "t", bump, &c, 10u, 0u) >= 0);

    EF_ASSERT_EQ_I(ef_sched_run(&s, 0u), 1);    /* next_due = 10 */
    /* The main loop was blocked until t = 105: nine slots missed. */
    EF_ASSERT_EQ_I(ef_sched_run(&s, 105u), 1);  /* runs ONCE, not 10 times */
    EF_ASSERT_EQ_I(c.calls, 2);
    EF_ASSERT_EQ_I(ef_sched_overruns(&s), 1);
    EF_ASSERT_EQ_I(ef_sched_task(&s, 0)->next_due, 115u);

    /* Back on time: no further overruns. */
    EF_ASSERT_EQ_I(ef_sched_run(&s, 115u), 1);
    EF_ASSERT_EQ_I(ef_sched_overruns(&s), 1);
}

EF_TEST(late_by_less_than_a_period_is_not_an_overrun)
{
    ef_sched s;
    counter c = { 0u, 0u };
    ef_sched_init(&s);
    EF_ASSERT(ef_sched_add(&s, "t", bump, &c, 10u, 0u) >= 0);
    (void)ef_sched_run(&s, 0u);         /* next_due = 10 */
    (void)ef_sched_run(&s, 19u);        /* 9 ticks late: jitter, not overrun */
    EF_ASSERT_EQ_I(ef_sched_overruns(&s), 0);
    (void)ef_sched_run(&s, 39u);        /* 10 ticks late: overrun */
    EF_ASSERT_EQ_I(ef_sched_overruns(&s), 1);
}

/* The tick counter wraps after 2^32 ticks -- about 49.7 days at 1 kHz. A
 * scheduler comparing `now >= next_due` unsigned would stop running every task
 * for another 49 days at that moment. This drives the counter straight through
 * the seam. */
EF_TEST(survives_the_tick_counter_wrap)
{
    ef_sched s;
    counter c = { 0u, 0u };
    uint32_t t;
    const uint32_t start = 0xFFFFFF00u;
    uint32_t expected = 0u;

    ef_sched_init(&s);
    EF_ASSERT(ef_sched_add(&s, "t", bump, &c, 10u, start) >= 0);

    /* 512 ticks straddling 0xFFFFFFFF -> 0x00000000. */
    for (t = 0u; t < 512u; t++) {
        uint32_t now = start + t;
        if (ef_sched_run(&s, now) == 1u) {
            expected++;
        }
    }
    /* 512 ticks at a period of 10, starting due: 52 runs (t = 0, 10, ..., 510). */
    EF_ASSERT_EQ_I(c.calls, 52);
    EF_ASSERT_EQ_I(expected, 52);
    EF_ASSERT_EQ_I(ef_sched_overruns(&s), 0);
}

/* A task whose first deadline is just past the wrap must not fire early. */
EF_TEST(deadline_past_the_wrap_is_respected)
{
    ef_sched s;
    counter c = { 0u, 0u };
    ef_sched_init(&s);
    /* now = 0xFFFFFFF0, first due at 0x00000010: 32 ticks in the future. */
    EF_ASSERT(ef_sched_add(&s, "t", bump, &c, 100u, 0x00000010u) >= 0);
    EF_ASSERT_EQ_I(ef_sched_run(&s, 0xFFFFFFF0u), 0);
    EF_ASSERT_EQ_I(ef_sched_run(&s, 0xFFFFFFFFu), 0);
    EF_ASSERT_EQ_I(ef_sched_run(&s, 0x0000000Fu), 0);
    EF_ASSERT_EQ_I(ef_sched_run(&s, 0x00000010u), 1);
}

int main(void)
{
    EF_RUN(add_validates);
    EF_RUN(nothing_runs_before_its_deadline);
    EF_RUN(period_is_honoured);
    EF_RUN(period_zero_runs_every_call);
    EF_RUN(all_due_tasks_run_each_call);
    EF_RUN(overrun_is_counted_not_replayed);
    EF_RUN(late_by_less_than_a_period_is_not_an_overrun);
    EF_RUN(survives_the_tick_counter_wrap);
    EF_RUN(deadline_past_the_wrap_is_respected);
    return ef_test_report("sched");
}
