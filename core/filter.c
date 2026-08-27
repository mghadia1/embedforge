#include "filter.h"

static bool is_pow2_u8(uint8_t v)
{
    return v != 0u && (v & (uint8_t)(v - 1u)) == 0u;
}

static uint8_t log2_u8(uint8_t v)
{
    uint8_t s = 0u;
    while ((uint8_t)(1u << s) != v) {
        s++;
    }
    return s;
}

bool ef_filter_init(ef_filter *f, uint8_t window, int32_t threshold,
                    uint8_t stable_needed)
{
    uint8_t i;

    if (f == NULL) {
        return false;
    }
    if (!is_pow2_u8(window) || window > (uint8_t)EF_FILTER_MAX_WINDOW) {
        return false;
    }
    if (stable_needed == 0u) {
        return false;
    }

    for (i = 0u; i < (uint8_t)EF_FILTER_MAX_WINDOW; i++) {
        f->window[i] = 0;
    }
    f->sum           = 0;
    f->len           = window;
    f->shift         = log2_u8(window);
    f->idx           = 0u;
    f->filled        = 0u;
    f->threshold     = threshold;
    f->stable_needed = stable_needed;
    f->stable_count  = 0u;
    f->state         = EF_FILTER_LOW;
    f->candidate     = EF_FILTER_LOW;
    f->samples       = 0u;
    f->transitions   = 0u;
    f->clamped       = 0u;
    return true;
}

ef_filter_state ef_filter_push(ef_filter *f, int32_t sample)
{
    ef_filter_state cand;
    int32_t avg;

    if (sample > (int32_t)EF_FILTER_SAMPLE_MAX) {
        sample = (int32_t)EF_FILTER_SAMPLE_MAX;
        f->clamped++;
    } else if (sample < -(int32_t)EF_FILTER_SAMPLE_MAX) {
        sample = -(int32_t)EF_FILTER_SAMPLE_MAX;
        f->clamped++;
    }

    /* Running sum: drop the sample leaving the window, add the new one.
     * Slots not yet written are 0, which is why `filled` tracks the real
     * divisor until the window saturates. */
    f->sum -= f->window[f->idx];
    f->window[f->idx] = sample;
    f->sum += sample;
    f->idx = (uint8_t)((f->idx + 1u) & (uint8_t)(f->len - 1u));
    if (f->filled < f->len) {
        f->filled++;
    }
    f->samples++;

    avg  = ef_filter_value(f);
    cand = (avg >= f->threshold) ? EF_FILTER_HIGH : EF_FILTER_LOW;

    if (cand == f->state) {
        /* Already there: nothing pending. */
        f->candidate    = cand;
        f->stable_count = 0u;
    } else if (cand == f->candidate) {
        f->stable_count++;
        if (f->stable_count >= f->stable_needed) {
            f->state        = cand;
            f->stable_count = 0u;
            f->transitions++;
        }
    } else {
        /* Candidate flipped before it could hold: restart the count. This
         * sample is the first of the new run, hence 1 and not 0. */
        f->candidate    = cand;
        f->stable_count = 1u;
        if (f->stable_count >= f->stable_needed) {
            f->state        = cand;
            f->stable_count = 0u;
            f->transitions++;
        }
    }

    return f->state;
}

int32_t ef_filter_value(const ef_filter *f)
{
    int32_t n;
    int32_t sum;

    if (f->filled == 0u) {
        return 0;
    }

    sum = f->sum;
    if (f->filled == f->len) {
        int32_t half;

        /* A window of one is the identity, and it has to be handled before
         * anything computes `shift - 1`: shift is 0 there, and 0u - 1u is
         * 4294967295, which is not a legal shift count. (UndefinedBehaviour-
         * Sanitizer caught exactly that in an earlier version of this
         * function, where the guard sat one line too late. The window-of-one
         * test still passed, because the bad value was computed and then never
         * used -- which is precisely the kind of bug that survives testing and
         * changes behaviour under a new compiler.) */
        if (f->shift == 0u) {
            return sum;
        }

        /* Full window: the divisor is a power of two, so round half away from
         * zero and shift. The +/- half-divisor bias before an arithmetic shift
         * is what makes it symmetric rather than floor(). */
        half = (int32_t)1 << (f->shift - 1u);
        return (sum >= 0) ? ((sum + half) >> f->shift)
                          : -(((-sum) + half) >> f->shift);
    }

    /* Partial window: not a power of two, so a real divide. This runs at most
     * `len - 1` times in the filter's life, right after init. */
    n = (int32_t)f->filled;
    return (sum >= 0) ? ((sum + n / 2) / n) : -(((-sum) + n / 2) / n);
}

ef_filter_state ef_filter_state_get(const ef_filter *f)
{
    return f->state;
}

void ef_filter_set_threshold(ef_filter *f, int32_t threshold)
{
    f->threshold    = threshold;
    f->stable_count = 0u;
    f->candidate    = f->state;
}

int32_t ef_filter_threshold(const ef_filter *f)
{
    return f->threshold;
}
