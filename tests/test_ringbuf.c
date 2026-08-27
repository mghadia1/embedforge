/* Ring buffer: the boundaries (empty, full, wrap) are exercised exhaustively,
 * because those are exactly the cases a hand-rolled index calculation gets
 * wrong and the ones an ISR will find in the field. */
#include <string.h>

#include "ef_test.h"
#include "ringbuf.h"

#define CAP 8u
static uint8_t storage[CAP];

static ef_ringbuf make(void)
{
    ef_ringbuf rb;
    memset(storage, 0, sizeof storage);
    EF_ASSERT(ef_rb_init(&rb, storage, CAP));
    return rb;
}

EF_TEST(init_rejects_bad_capacity)
{
    ef_ringbuf rb;
    EF_ASSERT(!ef_rb_init(&rb, storage, 0u));
    EF_ASSERT(!ef_rb_init(&rb, storage, 1u));   /* below the minimum of 2 */
    EF_ASSERT(!ef_rb_init(&rb, storage, 3u));   /* not a power of two     */
    EF_ASSERT(!ef_rb_init(&rb, storage, 100u));
    EF_ASSERT(!ef_rb_init(&rb, NULL, CAP));
    EF_ASSERT(ef_rb_init(&rb, storage, 2u));
    EF_ASSERT(ef_rb_init(&rb, storage, 32768u)); /* the documented maximum */
}

EF_TEST(empty_at_start)
{
    ef_ringbuf rb = make();
    uint8_t b = 0xEE;
    EF_ASSERT(ef_rb_is_empty(&rb));
    EF_ASSERT(!ef_rb_is_full(&rb));
    EF_ASSERT_EQ_I(ef_rb_count(&rb), 0);
    EF_ASSERT_EQ_I(ef_rb_capacity(&rb), CAP);
    EF_ASSERT(!ef_rb_pop(&rb, &b));
    EF_ASSERT_EQ_I(b, 0xEE); /* pop on empty must not touch the output */
}

EF_TEST(fifo_order)
{
    ef_ringbuf rb = make();
    uint8_t b;
    unsigned i;
    for (i = 0u; i < 5u; i++) {
        EF_ASSERT(ef_rb_push(&rb, (uint8_t)(0x10u + i)));
    }
    EF_ASSERT_EQ_I(ef_rb_count(&rb), 5);
    for (i = 0u; i < 5u; i++) {
        EF_ASSERT(ef_rb_pop(&rb, &b));
        EF_ASSERT_EQ_I(b, 0x10u + i);
    }
    EF_ASSERT(ef_rb_is_empty(&rb));
}

/* The full boundary: capacity bytes fit, capacity+1 does not, and the
 * rejected push neither corrupts the queue nor is silently forgotten. */
EF_TEST(full_boundary)
{
    ef_ringbuf rb = make();
    uint8_t b;
    unsigned i;

    for (i = 0u; i < CAP; i++) {
        EF_ASSERT(ef_rb_push(&rb, (uint8_t)i));
    }
    EF_ASSERT(ef_rb_is_full(&rb));
    EF_ASSERT_EQ_I(ef_rb_count(&rb), CAP);

    EF_ASSERT(!ef_rb_push(&rb, 0xFFu));
    EF_ASSERT(!ef_rb_push(&rb, 0xFFu));
    EF_ASSERT_EQ_I(ef_rb_drops(&rb), 2);
    EF_ASSERT_EQ_I(ef_rb_count(&rb), CAP);

    /* Contents survived the rejected pushes intact. */
    for (i = 0u; i < CAP; i++) {
        EF_ASSERT(ef_rb_pop(&rb, &b));
        EF_ASSERT_EQ_I(b, i);
    }
    EF_ASSERT(ef_rb_is_empty(&rb));

    /* One slot freed, one push accepted. */
    EF_ASSERT(ef_rb_push(&rb, 0x77u));
    EF_ASSERT_EQ_I(ef_rb_count(&rb), 1);
}

/* Drive the queue through many wraps at every possible fill level. This is the
 * test that would catch an off-by-one in the mask or a "waste one slot"
 * regression. */
EF_TEST(wrap_at_every_fill_level)
{
    unsigned fill;
    for (fill = 1u; fill <= CAP; fill++) {
        ef_ringbuf rb = make();
        unsigned round;
        uint8_t next_write = 0u;
        uint8_t next_read  = 0u;

        for (round = 0u; round < 200u; round++) {
            unsigned k;
            for (k = 0u; k < fill; k++) {
                EF_ASSERT(ef_rb_push(&rb, next_write++));
            }
            EF_ASSERT_EQ_I(ef_rb_count(&rb), fill);
            for (k = 0u; k < fill; k++) {
                uint8_t got;
                EF_ASSERT(ef_rb_pop(&rb, &got));
                EF_ASSERT_EQ_I(got, next_read++);
            }
            EF_ASSERT(ef_rb_is_empty(&rb));
        }
        EF_ASSERT_EQ_I(ef_rb_drops(&rb), 0);
    }
}

/* The uint16_t indices are free-running. Push and pop more than 65536 bytes so
 * both wrap through zero, and confirm nothing hiccups at the seam. */
EF_TEST(index_counter_wraps_past_65536)
{
    ef_ringbuf rb = make();
    unsigned long i;
    uint8_t expect = 0u;

    for (i = 0ul; i < 200000ul; i++) {
        uint8_t got;
        EF_ASSERT(ef_rb_push(&rb, (uint8_t)(i & 0xFFu)));
        EF_ASSERT(ef_rb_pop(&rb, &got));
        EF_ASSERT_EQ_I(got, expect++);
    }
    EF_ASSERT(ef_rb_is_empty(&rb));
    EF_ASSERT_EQ_I(ef_rb_drops(&rb), 0);
}

/* Interleaved partial drains: the producer stays ahead of the consumer by a
 * varying margin, which is how the ISR/main-loop pair actually behaves. */
EF_TEST(interleaved_partial_drain)
{
    ef_ringbuf rb = make();
    ef_rng rng = { 0xC0FFEEu };
    uint8_t next_write = 0u;
    uint8_t next_read  = 0u;
    unsigned outstanding = 0u;
    unsigned step;

    for (step = 0u; step < 20000u; step++) {
        unsigned want_push = ef_rng_next(&rng) % (CAP + 2u);
        unsigned want_pop  = ef_rng_next(&rng) % (CAP + 2u);
        unsigned k;

        for (k = 0u; k < want_push; k++) {
            if (ef_rb_push(&rb, next_write)) {
                next_write++;
                outstanding++;
            } else {
                EF_ASSERT_EQ_I(outstanding, CAP); /* only ever full */
            }
        }
        for (k = 0u; k < want_pop; k++) {
            uint8_t got;
            if (ef_rb_pop(&rb, &got)) {
                EF_ASSERT_EQ_I(got, next_read++);
                outstanding--;
            } else {
                EF_ASSERT_EQ_I(outstanding, 0); /* only ever empty */
            }
        }
        EF_ASSERT_EQ_I(ef_rb_count(&rb), outstanding);
    }
}

EF_TEST(pop_n_bulk_drain)
{
    ef_ringbuf rb = make();
    uint8_t out[16];
    unsigned i;

    for (i = 0u; i < CAP; i++) {
        EF_ASSERT(ef_rb_push(&rb, (uint8_t)(i * 3u)));
    }
    EF_ASSERT_EQ_I(ef_rb_pop_n(&rb, out, 3u), 3);
    for (i = 0u; i < 3u; i++) {
        EF_ASSERT_EQ_I(out[i], i * 3u);
    }
    /* Asking for more than is present returns what is there, not an error. */
    EF_ASSERT_EQ_I(ef_rb_pop_n(&rb, out, sizeof out), CAP - 3u);
    EF_ASSERT(ef_rb_is_empty(&rb));
    EF_ASSERT_EQ_I(ef_rb_pop_n(&rb, out, sizeof out), 0);
}

EF_TEST(reset_clears_contents_and_drops)
{
    ef_ringbuf rb = make();
    uint8_t b;
    unsigned i;
    for (i = 0u; i < CAP + 3u; i++) {
        (void)ef_rb_push(&rb, (uint8_t)i);
    }
    EF_ASSERT_EQ_I(ef_rb_drops(&rb), 3);
    ef_rb_reset(&rb);
    EF_ASSERT(ef_rb_is_empty(&rb));
    EF_ASSERT_EQ_I(ef_rb_drops(&rb), 0);
    EF_ASSERT(!ef_rb_pop(&rb, &b));
}

/* Every byte value round-trips, including 0x00 and 0xFF. */
EF_TEST(all_byte_values_round_trip)
{
    ef_ringbuf rb = make();
    unsigned v;
    for (v = 0u; v <= 255u; v++) {
        uint8_t got;
        EF_ASSERT(ef_rb_push(&rb, (uint8_t)v));
        EF_ASSERT(ef_rb_pop(&rb, &got));
        EF_ASSERT_EQ_I(got, v);
    }
}

int main(void)
{
    EF_RUN(init_rejects_bad_capacity);
    EF_RUN(empty_at_start);
    EF_RUN(fifo_order);
    EF_RUN(full_boundary);
    EF_RUN(wrap_at_every_fill_level);
    EF_RUN(index_counter_wraps_past_65536);
    EF_RUN(interleaved_partial_drain);
    EF_RUN(pop_n_bulk_drain);
    EF_RUN(reset_clears_contents_and_drops);
    EF_RUN(all_byte_values_round_trip);
    return ef_test_report("ringbuf");
}
