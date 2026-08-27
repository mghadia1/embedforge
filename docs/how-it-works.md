# How EmbedForge works

This document explains the four decisions that matter: why the ring buffer is
safe to share with an interrupt, how the filter avoids floating point without
overflowing, how the scheduler survives a counter wrap, and where everything
lives in memory.

---

## 1. The SPSC ring buffer, and why it is ISR-safe

`core/ringbuf.c` is a fixed-capacity byte queue shared between two contexts:

- the **producer** is the UART receive ISR (`UART0RX_Handler` in `bsp/uart.c`),
  which pushes one byte per interrupt;
- the **consumer** is the `comms` task on the main loop, which pops.

There is no lock, no interrupt masking, and no compare-and-swap. Three separate
properties make that safe, and all three are required.

### 1.1 Exactly one writer per index

The queue holds two indices. **The producer only ever writes `head`. The
consumer only ever writes `tail`.** Each reads the other's index but never
modifies it.

This is what removes the need for mutual exclusion. A read-modify-write on a
shared variable is only atomic if nothing can interrupt it — and an ISR
interrupts the main loop by definition. But an index with a single writer is
never read-modify-written by two parties, so there is no window to protect. The
producer's `head + 1` is safe because nobody else can be halfway through their
own `head + 1`.

The cost of that guarantee is the constraint in the header: **one producer, one
consumer.** Two ISRs pushing into the same queue would break it immediately, and
no amount of `volatile` would help. That is a design contract, not an
implementation detail, and it is why the type is named for it.

### 1.2 Memory ordering, not just `volatile`

`volatile` tells the compiler not to cache or reorder accesses to a variable.
It says nothing whatsoever about the order in which those accesses become
*visible*, and nothing about the ordering of the surrounding non-volatile
accesses — including the byte being stored into the buffer.

The dangerous interleaving is:

```
producer:  buf[head] = byte;      (1)
           head = head + 1;       (2)
consumer:  if (head != tail)      (3)
               byte = buf[tail];  (4)
```

If (2) becomes visible before (1), the consumer can pass the check at (3) and
read the slot at (4) before the byte was actually written. On a Cortex-M3 this
is mostly a compiler-reordering risk rather than a hardware one, but "mostly"
is not a specification, and the same code is compiled for the host tests where
it is a genuine hardware risk.

So the indices are C11 atomics and the ordering is stated explicitly:

```c
/* producer */
rb->buf[head & rb->mask] = byte;
atomic_store_explicit(&rb->head, head + 1, memory_order_release);

/* consumer */
head = atomic_load_explicit(&rb->head, memory_order_acquire);
...
*out = rb->buf[tail & rb->mask];
```

The **release** store guarantees the byte write is visible to anyone who
observes the new `head`. The matching **acquire** load guarantees that a
consumer which sees the new `head` also sees the byte. The reverse pair on
`tail` stops the producer from overwriting a slot the consumer has not finished
reading.

On ARMv7-M this compiles to a `dmb` plus an ordinary store — a few cycles in an
ISR that runs for a few dozen. On the host it is what makes the tests mean
something under a thread sanitizer.

Reading each index from its *own* side uses `memory_order_relaxed`: nobody else
writes it, so there is nothing to synchronise with.

### 1.3 Free-running indices and the full/empty distinction

The indices are free-running `uint16_t` counters, masked only when used to
address the buffer:

```c
count = (uint16_t)(head - tail);
slot  = buf[head & mask];
```

This distinguishes full from empty without the usual trick of wasting a slot:
empty is `head == tail`, full is `head - tail == capacity`.

It is exact only because unsigned wraparound is exact: `head` and `tail` wrap at
65536, and the capacity is a power of two that divides 65536, so the difference
stays correct across the wrap. That is why `ef_rb_init` rejects any capacity
that is not a power of two in `[2, 32768]` — the constraint is load-bearing, not
stylistic. `test_ringbuf.c` pushes 200,000 bytes through an 8-byte queue to
drive both counters through zero.

### 1.4 What happens when it fills

The producer is an ISR, so it cannot block and cannot wait. When the queue is
full, `ef_rb_push` drops the byte and increments a counter, which `STATS`
reports as `drop=`.

That is a deliberate choice: dropping input under overload is recoverable, and
a line protocol resynchronises at the next newline. The `overflow` QEMU
scenario exercises it for real — it sends 1,000 bytes at once into a 256-byte
queue, confirms exactly 256 bytes were accepted and 744 counted as dropped,
verifies the command straddling the loss is *rejected* rather than mis-parsed,
and then confirms the firmware still works.

Note that the queue overflowing is different from the *hardware* overrunning.
If the ISR itself fell behind, the UART would set its own overrun flag and the
byte would be lost before the queue ever saw it. That is counted separately, as
`ovr=`, because it means something different: `drop` says the main loop is too
slow, `ovr` says the interrupt is.

### 1.5 Why the ISR does almost nothing

The handler reads one byte and pushes it. No parsing, no formatting, no
transmit. Two reasons:

- **Latency.** The interrupt must finish before the next byte arrives (87 µs at
  115200 baud). Parsing in the ISR would risk the hardware overrun above.
- **Surface area.** Everything the ISR touches is shared state that needs a
  concurrency argument. Doing one push means there is exactly one shared
  structure, the one whose ordering is specified above. Parsing in the ISR would
  put the parser's state machine on that list too.

Transmit is polled from the main loop for the same reason in reverse: a TX
interrupt would add a second concurrency edge to buy latency the firmware does
not need.

---

## 2. The fixed-point filter

`core/filter.c`. No floating point anywhere in the image — not for
determinism-theatre reasons, but because a Cortex-M3 has no FPU, so every
`float` operation would be a libgcc software routine costing hundreds of cycles.

### 2.1 Moving average

A running sum, not a loop over the window:

```c
sum -= window[idx];      /* the sample leaving  */
window[idx] = sample;
sum += sample;           /* the sample arriving */
```

One add, one subtract, one store per sample, regardless of window size. The
divide is a shift because the window is constrained to a power of two.

**Before the window fills**, the average is taken over the samples actually
seen, not over the zeros still in the array. A plain `sum >> shift` would report
25 after a single sample of 100, which makes the first fraction of a second
after power-up a lie. The partial case costs a real division, and it runs at
most `window - 1` times in the filter's life.

**Rounding is half-away-from-zero**, so `+5/2` is `+3` and `-5/2` is `-3`. An
arithmetic shift alone would floor, biasing every negative reading downward —
invisible on a positive-only sensor and wrong the moment the signal crosses
zero.

### 2.2 Overflow is bounded by construction

The accumulator is `int32_t`. Rather than hoping it never overflows:

- samples are clamped to ±2²⁰ (`EF_FILTER_SAMPLE_MAX`) on the way in, and the
  clamp is counted;
- the window is at most 64 (`EF_FILTER_MAX_WINDOW`), rejected at `init`.

So `|sum| ≤ 2²⁰ × 2⁶ = 2²⁶`, comfortably inside `int32_t`, and the bound holds
for *every* input rather than for expected inputs. `test_filter.c` drives 50,000
worst-case alternating extreme samples through the largest window and asserts
the bound directly.

### 2.3 Debounce

The averaged value is compared to a threshold to give a candidate state. The
reported state only changes after the candidate has held for N consecutive
samples.

Without this, a signal sitting on the threshold produces a transition per
sample. `dithering_signal_never_transitions` feeds 500 samples alternating one
unit either side of the threshold and asserts **zero** transitions.

The averaging and the debounce do different jobs and neither replaces the other:
the average suppresses noise *within* a reading, the debounce suppresses
oscillation *between* readings.

---

## 3. The cooperative scheduler

`core/sched.c`. A table of tasks, each with a period in ticks. The main loop is:

```c
for (;;) {
    ef_sched_run(&scheduler, bsp_tick_get());
}
```

No preemption, no per-task stacks, no context switching. A task runs to
completion on the main stack — which is why the whole system has exactly one
stack and a single high-water measurement covers all of it.

### 3.1 The tick wrap

The tick counter is a free-running `uint32_t` incremented by SysTick at 1 kHz.
It wraps every 2³² ms ≈ **49.7 days**.

The obvious comparison is wrong:

```c
if (now >= task->next_due)      /* WRONG */
```

At the wrap, `now` becomes small while `next_due` is still huge, so every task
stops running — for another 49.7 days. It is the kind of bug that passes every
test and takes down a device after seven weeks of uptime.

The code compares the *signed difference* instead:

```c
if ((int32_t)(now - task->next_due) >= 0)
```

The subtraction wraps modulo 2³², and reinterpreting the result as signed gives
the correct relative ordering as long as no deadline is more than 2³¹ ticks
(~24 days) away — which no period in this firmware approaches.
`survives_the_tick_counter_wrap` drives the counter straight through
`0xFFFFFFFF → 0x00000000` and asserts the task keeps its cadence.

### 3.2 Overrun policy

If a task misses deadlines because something else ran long, the scheduler does
**not** catch up. It counts the overrun and re-bases the next deadline on *now*:

```c
if ((uint32_t)(now - t->next_due) >= t->period) {
    t->overruns++;
}
t->next_due = now + t->period;
```

The naive `next_due += period` would replay every missed slot — turning one slow
task into a burst that starves everything behind it, exactly when the system is
already overloaded. Abandoning missed slots and counting them keeps the failure
visible instead of amplifying it. `STATS` reports the total as `sovr=`.

That counter is genuinely useful and genuinely non-portable: it reflects host
load under emulation, which is why the QEMU fixtures mask it rather than
asserting a fixed value.

### 3.3 Bounded work per task

`task_comms` drains at most 64 bytes per run. Unbounded draining would let a
flood of input keep one task running long enough to make the others miss their
deadlines — the backlog stays in the queue, which is what the queue is for.

---

## 4. Memory map

Defined by [`bsp/linker.ld`](../bsp/linker.ld) for the MPS2-AN385.

Sizes below are from GCC 16.2.0; `.text` moves by a few dozen bytes on a
different compiler (CI's GCC 13.2.1 gives 3,448 B). `make size` prints the
figures for whichever toolchain is installed.

```
0x00000000  FLASH, 128 KB budget
            .isr_vector    192 B    vector table: initial SP, then 15 core
                                    exceptions, then 32 external IRQs
            .text        3,416 B    code and read-only data
            (.data image)     0 B   initialisers copied to RAM at reset

0x20000000  RAM, 32 KB budget
            .data             0 B   initialised globals
            .bss            864 B   zeroed globals: queue storage, parser,
                                    filter, task table
            .stack        4,096 B   the one and only stack
```

The RAM side is compiler-independent: it is fixed by the data structures and
the reserved stack, not by codegen.

The board actually has 4 MB at each address. The budgets are deliberately much
smaller — a realistic size for a part this firmware would ship on — so that the
footprint is **enforced**: exceeding it fails the link. The stack is a declared
section rather than "whatever is left", so globals and stack cannot silently
collide the first time the call depth grows.

### 4.1 Reset sequence

The Cortex-M3 loads SP from the first word of the vector table and the reset
vector from the second, in hardware, before executing anything. Then
`Reset_Handler` (`bsp/startup.c`) does what a C library's startup would, if
there were one:

1. copy `.data` initialisers from flash to RAM;
2. zero `.bss` — nothing may read a global before this line;
3. paint the unused stack region with `0xA5A5A5A5`;
4. call `main()`.

It is written in C rather than assembly because the only part that genuinely
needs assembly — loading the initial SP — is done by the core itself.

### 4.2 Measuring the stack, rather than estimating it

Step 3 is what makes `stackfree` a measurement. `bsp_stack_free()` counts intact
paint words upward from the bottom of the region; the first overwritten word is
the deepest the stack has ever reached since reset.

The paint loop stops 64 bytes below the current frame — painting over the frame
you are standing in would corrupt the return address — so the reported figure is
slightly pessimistic, which is the safe direction.

### 4.3 No heap

There is no `.heap` section, no `_sbrk`, and no `_end` symbol. The image is
linked `-nostdlib`, so there is no C library present to pull an allocator in;
the two functions the compiler may synthesise calls to (`memcpy`, `memset`) are
provided by hand in `bsp/libc_shim.c`. `make no-heap` verifies all three
independently.

---

## 5. What the split does and does not prove

Roughly nine tenths of the logic lives in `core/` and is covered by 61 host test
cases and 5.25 M assertions, run under ASan and UBSan in CI. That is a strong
claim, and it is the claim being made.

What it does not cover is `bsp/` — the register writes, the vector table, the
interrupt acknowledgement. No host test can reach that code. It is covered
instead by the QEMU integration run, which is emulation and is labelled as
emulation, and by keeping the layer small enough to read.

The value of that boundary is not theoretical. The one bug that neither the
host tests nor the type system could have caught — the ISR clearing its
interrupt flag one line too late — was in `bsp/uart.c`, in the twelve lines the
host tests cannot reach, and it was found by running the firmware. That is the
argument for having both halves, stated as precisely as the project can state
it.
