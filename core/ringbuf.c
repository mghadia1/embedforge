#include "ringbuf.h"

static bool is_pow2(uint16_t v)
{
    return v != 0u && (v & (uint16_t)(v - 1u)) == 0u;
}

bool ef_rb_init(ef_ringbuf *rb, uint8_t *storage, uint16_t capacity)
{
    if (rb == NULL || storage == NULL) {
        return false;
    }
    /* Lower bound 2 keeps "full" distinguishable from "empty"; upper bound
     * 32768 keeps capacity a divisor of 65536 so the free-running uint16_t
     * indices wrap on an exact multiple of the capacity. */
    if (!is_pow2(capacity) || capacity < 2u || capacity > 32768u) {
        return false;
    }
    rb->buf  = storage;
    rb->mask = (uint16_t)(capacity - 1u);
    atomic_init(&rb->head, (uint16_t)0);
    atomic_init(&rb->tail, (uint16_t)0);
    atomic_init(&rb->drops, (uint32_t)0);
    return true;
}

void ef_rb_reset(ef_ringbuf *rb)
{
    atomic_store_explicit(&rb->head, (uint16_t)0, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, (uint16_t)0, memory_order_relaxed);
    atomic_store_explicit(&rb->drops, (uint32_t)0, memory_order_relaxed);
}

bool ef_rb_push(ef_ringbuf *rb, uint8_t byte)
{
    /* `head` is ours: nobody else writes it, so a relaxed load is enough. */
    const uint16_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    /* `tail` moves under us. Acquire pairs with the consumer's release store,
     * guaranteeing that the slot it freed is really free before we reuse it. */
    const uint16_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    if ((uint16_t)(head - tail) > rb->mask) { /* == capacity: full */
        atomic_fetch_add_explicit(&rb->drops, (uint32_t)1, memory_order_relaxed);
        return false;
    }

    rb->buf[head & rb->mask] = byte;
    /* Release: the byte store above is visible to any consumer that observes
     * this new head. Without it the consumer could read a stale slot. */
    atomic_store_explicit(&rb->head, (uint16_t)(head + 1u), memory_order_release);
    return true;
}

bool ef_rb_pop(ef_ringbuf *rb, uint8_t *out)
{
    const uint16_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    const uint16_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

    if (head == tail) {
        return false;
    }

    *out = rb->buf[tail & rb->mask];
    /* Release: the read above completes before the producer sees the slot
     * become free, so it cannot overwrite a byte we have not taken yet. */
    atomic_store_explicit(&rb->tail, (uint16_t)(tail + 1u), memory_order_release);
    return true;
}

uint16_t ef_rb_pop_n(ef_ringbuf *rb, uint8_t *dst, uint16_t max)
{
    uint16_t n = 0u;
    while (n < max) {
        if (!ef_rb_pop(rb, &dst[n])) {
            break;
        }
        n++;
    }
    return n;
}

uint16_t ef_rb_capacity(const ef_ringbuf *rb)
{
    return (uint16_t)(rb->mask + 1u);
}

uint16_t ef_rb_count(const ef_ringbuf *rb)
{
    const uint16_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    const uint16_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    return (uint16_t)(head - tail);
}

bool ef_rb_is_empty(const ef_ringbuf *rb)
{
    return ef_rb_count(rb) == 0u;
}

bool ef_rb_is_full(const ef_ringbuf *rb)
{
    return ef_rb_count(rb) == ef_rb_capacity(rb);
}

uint32_t ef_rb_drops(const ef_ringbuf *rb)
{
    return atomic_load_explicit(&rb->drops, memory_order_relaxed);
}
