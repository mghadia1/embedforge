/* sched.h — cooperative super-loop scheduler.
 *
 * No RTOS, no stacks per task, no preemption. A fixed table of tasks, each with
 * a period in ticks; ef_sched_run() is called from the main loop with the
 * current tick and runs whatever is due. A task runs to completion on the main
 * stack, so the only stack depth in the system is main's -- which is why the
 * firmware can prove its RAM ceiling.
 *
 * Tick arithmetic is wraparound-safe. The tick counter is a free-running
 * uint32_t that wraps after 2^32 ticks (~49.7 days at 1 kHz). Comparing
 * `now >= next_due` directly would break at that wrap and stall every task for
 * another 49 days; the code instead tests
 *
 *     (int32_t)(now - next_due) >= 0
 *
 * which is correct as long as no task is more than 2^31 ticks overdue. See
 * test_sched.c, which drives the counter across the wrap.
 *
 * Overrun policy: if a task misses deadlines (because an earlier task ran
 * long), the scheduler does NOT try to catch up by running it repeatedly. The
 * next deadline is set from `now`, the missed slots are abandoned, and an
 * overrun counter is incremented. Catching up would turn one slow task into a
 * burst that starves everything else -- the classic failure mode of a naive
 * `next_due += period`. The counter is what you look at to size the periods.
 */
#ifndef EF_SCHED_H
#define EF_SCHED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ef_config.h"

typedef void (*ef_task_fn)(void *ctx);

typedef struct {
    ef_task_fn  fn;
    void       *ctx;
    const char *name;
    uint32_t    period;    /* ticks between runs; 0 means every call */
    uint32_t    next_due;  /* absolute tick, wraparound-safe          */
    uint32_t    runs;
    uint32_t    overruns;  /* times the task was late by >= 1 period  */
} ef_task;

typedef struct {
    ef_task tasks[EF_SCHED_MAX_TASKS];
    uint8_t count;
} ef_sched;

/* Empty the table. `now` seeds the first deadlines of later ef_sched_add()
 * calls. */
void ef_sched_init(ef_sched *s);

/* Append a task due first at `first_due`. Returns the task index, or -1 if the
 * table is full or `fn` is NULL. */
int ef_sched_add(ef_sched *s, const char *name, ef_task_fn fn, void *ctx,
                 uint32_t period, uint32_t first_due);

/* Run every task whose deadline has passed, in table order. Returns how many
 * ran. Each task runs at most once per call. */
uint8_t ef_sched_run(ef_sched *s, uint32_t now);

const ef_task *ef_sched_task(const ef_sched *s, uint8_t index);

/* Sum of every task's overrun counter. */
uint32_t ef_sched_overruns(const ef_sched *s);

#endif /* EF_SCHED_H */
