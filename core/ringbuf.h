/* ringbuf.h — lock-free single-producer / single-consumer byte queue.
 *
 * Concurrency contract (this is the whole point of the type):
 *
 *   - Exactly ONE producer calls ef_rb_push(). In this firmware that is the
 *     UART receive ISR.
 *   - Exactly ONE consumer calls ef_rb_pop() / ef_rb_pop_n(). In this firmware
 *     that is the main super-loop.
 *   - The producer only ever writes `head`; the consumer only ever writes
 *     `tail`. Neither index is read-modify-written by both sides, so no
 *     compare-and-swap and no interrupt masking is required.
 *
 * Both indices are free-running uint16_t counters that are masked on use. That
 * costs no slot: the queue is full when (head - tail) == capacity and empty
 * when head == tail, and unsigned wraparound at 65536 is exact because the
 * capacity is a power of two that divides 65536.
 *
 * Ordering is expressed with C11 atomics rather than bare `volatile`:
 * the producer publishes the byte with a release store to `head`, the consumer
 * acquires it with an acquire load, and vice versa for `tail`. On Cortex-M
 * this compiles to a `dmb` plus an ordinary store; on the host it is what makes
 * the tests meaningful under a thread sanitizer. `volatile` alone would stop
 * the compiler reordering but says nothing about the memory model.
 *
 * ef_rb_init() is NOT concurrency-safe and must complete before either side
 * runs. No function here allocates.
 */
#ifndef EF_RINGBUF_H
#define EF_RINGBUF_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;             /* caller-owned storage, >= capacity bytes  */
    uint16_t mask;            /* capacity - 1                             */
    _Atomic uint16_t head;    /* producer writes, consumer reads          */
    _Atomic uint16_t tail;    /* consumer writes, producer reads          */
    _Atomic uint32_t drops;   /* producer-only: pushes rejected when full */
} ef_ringbuf;

/* Bind `storage` (at least `capacity` bytes) to `rb`.
 * `capacity` must be a power of two in [2, 32768]. Returns false and leaves
 * `rb` unusable otherwise. */
bool ef_rb_init(ef_ringbuf *rb, uint8_t *storage, uint16_t capacity);

/* Discard all buffered bytes and the drop counter. Not safe to call while
 * either side is running. */
void ef_rb_reset(ef_ringbuf *rb);

/* Producer side. Returns false and increments the drop counter if full. */
bool ef_rb_push(ef_ringbuf *rb, uint8_t byte);

/* Consumer side. Returns false if empty; *out is untouched in that case. */
bool ef_rb_pop(ef_ringbuf *rb, uint8_t *out);

/* Consumer side bulk drain. Copies at most `max` bytes into `dst`, returns the
 * number copied. */
uint16_t ef_rb_pop_n(ef_ringbuf *rb, uint8_t *dst, uint16_t max);

/* Capacity in bytes. */
uint16_t ef_rb_capacity(const ef_ringbuf *rb);

/* Bytes currently readable. Exact when called from the consumer; from the
 * producer it is a lower bound, and from a third party it is a hint. */
uint16_t ef_rb_count(const ef_ringbuf *rb);

bool ef_rb_is_empty(const ef_ringbuf *rb);
bool ef_rb_is_full(const ef_ringbuf *rb);

/* Total pushes rejected because the queue was full, since init/reset. */
uint32_t ef_rb_drops(const ef_ringbuf *rb);

#endif /* EF_RINGBUF_H */
