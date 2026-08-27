#include "sched.h"

void ef_sched_init(ef_sched *s)
{
    s->count = 0u;
}

int ef_sched_add(ef_sched *s, const char *name, ef_task_fn fn, void *ctx,
                 uint32_t period, uint32_t first_due)
{
    ef_task *t;

    if (fn == NULL || s->count >= (uint8_t)EF_SCHED_MAX_TASKS) {
        return -1;
    }

    t = &s->tasks[s->count];
    t->fn       = fn;
    t->ctx      = ctx;
    t->name     = name;
    t->period   = period;
    t->next_due = first_due;
    t->runs     = 0u;
    t->overruns = 0u;

    return (int)(s->count++);
}

uint8_t ef_sched_run(ef_sched *s, uint32_t now)
{
    uint8_t ran = 0u;
    uint8_t i;

    for (i = 0u; i < s->count; i++) {
        ef_task *t = &s->tasks[i];

        /* Signed difference: correct across the uint32_t wrap. */
        if ((int32_t)(now - t->next_due) < 0) {
            continue;
        }

        t->fn(t->ctx);
        t->runs++;
        ran++;

        if (t->period == 0u) {
            t->next_due = now;
            continue;
        }

        /* Late by a whole period or more? Count it and resynchronise to now
         * instead of replaying the missed slots. */
        if ((uint32_t)(now - t->next_due) >= t->period) {
            t->overruns++;
        }
        t->next_due = now + t->period;
    }

    return ran;
}

const ef_task *ef_sched_task(const ef_sched *s, uint8_t index)
{
    if (index >= s->count) {
        return NULL;
    }
    return &s->tasks[index];
}

uint32_t ef_sched_overruns(const ef_sched *s)
{
    uint32_t total = 0u;
    uint8_t i;
    for (i = 0u; i < s->count; i++) {
        total += s->tasks[i].overruns;
    }
    return total;
}
