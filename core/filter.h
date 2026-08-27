/* filter.h — fixed-point sensor conditioning: moving average + debounce.
 *
 * There is no floating point anywhere in this firmware. Samples are integers
 * in whatever milli-unit the sensor reports; the filter never changes scale.
 *
 * Two stages:
 *
 *  1. Moving average over a power-of-two window. A running sum is kept, so a
 *     sample costs one add, one subtract and one shift -- no loop over the
 *     window. The divide is a shift because the window is a power of two.
 *
 *  2. Debounce. The averaged value is compared against a threshold to give a
 *     candidate binary state. The reported state only changes after the
 *     candidate has held for `stable_needed` consecutive samples, so a signal
 *     dithering around the threshold does not produce a burst of transitions.
 *
 * Overflow is bounded by construction, not by hope: samples are clamped to
 * +/- EF_FILTER_SAMPLE_MAX (2^20) and the window is at most
 * EF_FILTER_MAX_WINDOW (64), so |sum| <= 2^26 and the accumulator cannot
 * overflow int32_t. ef_filter_init() rejects any configuration outside that.
 *
 * Rounding: the average rounds half away from zero, so +5/2 -> +3 and
 * -5/2 -> -3. That is symmetric about zero, which matters for a signed sensor
 * (an arithmetic-shift-only implementation would floor and bias negatives
 * downward). See test_filter.c.
 */
#ifndef EF_FILTER_H
#define EF_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ef_config.h"

typedef enum {
    EF_FILTER_LOW  = 0,
    EF_FILTER_HIGH = 1
} ef_filter_state;

typedef struct {
    int32_t window[EF_FILTER_MAX_WINDOW];
    int32_t sum;
    uint8_t len;            /* window size, power of two, <= EF_FILTER_MAX_WINDOW */
    uint8_t shift;          /* log2(len)                                          */
    uint8_t idx;            /* next slot to overwrite                             */
    uint8_t filled;         /* samples seen, saturating at len                    */

    int32_t threshold;      /* average >= threshold means candidate HIGH          */
    uint8_t stable_needed;  /* consecutive agreeing samples required to switch    */
    uint8_t stable_count;
    ef_filter_state state;
    ef_filter_state candidate;

    uint32_t samples;       /* total samples accepted                             */
    uint32_t transitions;   /* debounced state changes                            */
    uint32_t clamped;       /* samples that hit the +/- SAMPLE_MAX clamp          */
} ef_filter;

/* Configure the filter. `window` must be a power of two in
 * [1, EF_FILTER_MAX_WINDOW]; `stable_needed` must be >= 1. Returns false and
 * leaves the filter unusable otherwise. Starts in EF_FILTER_LOW with an empty
 * window. */
bool ef_filter_init(ef_filter *f, uint8_t window, int32_t threshold,
                    uint8_t stable_needed);

/* Feed one raw sample. Returns the new debounced state. */
ef_filter_state ef_filter_push(ef_filter *f, int32_t sample);

/* Moving average of the samples seen so far. Before the window has filled, the
 * average is over the samples actually seen, not over zeros -- so the first
 * reading is the first sample rather than sample/window. */
int32_t ef_filter_value(const ef_filter *f);

ef_filter_state ef_filter_state_get(const ef_filter *f);

/* Change the threshold at runtime (the SET command). Does not clear the
 * window; the next sample re-evaluates the candidate against the new
 * threshold, and the debounce counter restarts. */
void ef_filter_set_threshold(ef_filter *f, int32_t threshold);

int32_t ef_filter_threshold(const ef_filter *f);

#endif /* EF_FILTER_H */
