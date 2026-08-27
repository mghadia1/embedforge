# EmbedForge — bare-metal Cortex-M firmware, provable on a host and runnable without a board

A telemetry and command firmware for an ARM Cortex-M3: bytes arrive on a UART
in an interrupt, cross into the main loop through a lock-free queue, are parsed
by a state machine into commands, and drive a fixed-point sensor filter under a
cooperative scheduler. C11, no RTOS, no dynamic allocation, 3.6 KB of flash.

The point of the project is not that it is large. It is that every claim on
this page is checkable by someone else, on their own machine, in about two
minutes:

```bash
make all
```

## Status: `resume_eligible: no` — the explanation gate is still open

Everything below is built, run and measured. What is not yet true is the
condition this portfolio puts on résumé use: Mayank can explain, unaided, why
the SPSC ring buffer is safe with a producer in an ISR, one memory or timing
constraint the design lives under, and one failure mode of the parser. Until
then this is a finished project, not a résumé line.

## What is actually verified

| Gate | Result |
|---|---|
| Host unit tests | **61 cases, 5.25 M assertions, all passing** |
| Same tests under ASan + UBSan | clean |
| Compiler warnings | `-Wall -Wextra -Werror` plus `-Wconversion -Wsign-conversion -Wshadow -Wcast-qual`, host and target |
| `cppcheck` | clean (`warning,style,performance,portability`) |
| Dynamic allocation | **none** — verified three ways, see below |
| Footprint | **3.6 KB flash / 4.8 KB RAM**, enforced by the linker script |
| QEMU integration | **5 scenarios passing**, guest exits 0 on its own |

### Footprint

From `arm-none-eabi-size -A` on the linked image (`make size`). Code size
depends on the compiler, so the toolchain is named rather than left implied:

| Section | GCC 16.2.0 | GCC 13.2.1 (CI) | |
|---|---:|---:|---|
| `.isr_vector` | 192 | 192 | vector table |
| `.text` | 3,416 | 3,448 | code and read-only data |
| `.data` | 0 | 0 | initialised globals |
| `.bss` | 864 | 864 | zeroed globals |
| `.stack` | 4,096 | 4,096 | reserved stack region |
| **FLASH** | **3,608** | **3,640** | of 131,072 (2.75% / 2.78%) |
| **RAM** | **4,960** | **4,960** | of 32,768 (15.14%) |

The RAM figure is identical across both, because it is fixed by the data
structures and the reserved stack rather than by codegen. Flash differs by
32 bytes. Neither number is quoted anywhere in this repo without the
compiler that produced it, and `make size` reprints them for whatever
toolchain you have.

The 128 KB / 32 KB budgets are not commentary — they are the `MEMORY` regions
in [`bsp/linker.ld`](bsp/linker.ld), and the stack is a real section rather than
"whatever is left over". Exceeding either budget fails the link instead of
colliding at runtime.

Of the 4 KB stack, the firmware's measured high-water mark leaves about 3.6 KB
untouched (it varies by a few dozen bytes with the compiler, for the same
reason `.text` does). That is a measurement, not an estimate: the reset
handler paints the region with a known pattern before `main()`, and `STATS`
reports how much of it has never been overwritten.

### The no-heap guarantee

`make no-heap` checks three independent things, because "we don't call malloc"
is a claim and this makes it a property of the artifact:

1. no allocation call appears anywhere in `core/`, `bsp/` or `app/`;
2. no allocator symbol survives into the linked image — it is built `-nostdlib`,
   so there is no C library to drag one in;
3. the linker script defines no heap region and no `_sbrk`, so there is nothing
   for an allocator to grow into even if one appeared.

A stray `malloc` fails the link rather than failing at three in the morning.

## The design rule that makes it testable

The code is split in two, and the split is the whole idea.

**`core/`** is portable freestanding C with no hardware in it at all: the ring
buffer, the parser, the filter, the scheduler, and the small amount of integer
formatting the firmware needs in place of `printf`. It compiles with native
`cc` and is covered by host unit tests that run in CI, under sanitizers, with
every random input seeded so a failure is reproducible from the seed.

**`bsp/`** is the thin hardware layer: UART, GPIO, SysTick, startup code and
linker script, built with `arm-none-eabi-gcc`. Its entire application-facing
interface is five things — a tick, a byte out, a byte in, an LED, a way to stop.

**`app/`** is the wiring: it owns the static storage, hands the ISR its sink,
builds the task table, and turns parsed commands into replies. Nothing else.

So the algorithmic correctness is proven on the host, and the hardware bring-up
is a separate and clearly-labelled step. That is the same discipline the ML
projects in this portfolio use, for the same reason.

## Running it without buying hardware

```bash
make firmware   # arm-none-eabi-gcc, prints the footprint
make qemu       # boots the image, drives the UART, checks the replies
```

QEMU runs the `mps2-an385` machine — a real Cortex-M3 core model with the CMSDK
UART and SysTick — and the image it boots is byte-for-byte the one that would be
flashed to a board.

**This is emulation, and it is labelled as emulation everywhere.** What the
QEMU run does establish: the image boots, the vector table and reset code are
correct, `.data` and `.bss` are set up, the UART driver and its receive
interrupt work, and the whole `core/` + `bsp/` stack behaves end to end under
scripted input. What it does not establish: anything about real-world timing,
electrical behaviour, or a real sensor. There is no sensor here; the streaming
mode reports a deterministic synthetic waveform, and says so in the source.

Five scenarios run: every command with its counters, the filter's averaging and
debounce end to end, malformed input and recovery, a 1,000-byte burst into a
256-byte queue, and the periodic tasks under the scheduler. Three are compared
byte-for-byte against committed fixtures; two assert on structure, for the
reason in the next section.

### The protocol

```
PING            -> PONG
GET             -> VAL <filtered value> <LOW|HIGH>
SET <int>       -> OK THRESH <int>        set the debounce threshold
FEED <int>      -> OK FEED <int>          inject one sensor sample
STREAM ON|OFF   -> OK STREAM ON|OFF       periodic reporting at 5 Hz
STATS           -> STATS rx= cmd= err= drop= ovr= sovr= stackfree=
QUIT            -> BYE, then shut down
```

Anything else is `ERR UNKNOWN`, `ERR ARG` or `ERR TOOLONG`. Try it by hand:

```bash
printf 'PING\nFEED 800\nGET\nSTATS\nQUIT\n' | qemu-system-arm -M mps2-an385 -nographic -semihosting-config enable=on,target=native -kernel build/firmware.elf
```

## Three bugs the process caught

These are in the README rather than buried in the history, because they are the
part of the project that is actually worth talking about.

**The ISR acknowledged its interrupt one line too late.** The receive handler
read the byte out of the UART and then cleared the interrupt flag. Reading the
byte frees the holding register, which lets the UART latch the *next* byte
immediately — and that byte raises its own interrupt, which the clear then
wiped. The firmware received exactly one byte and wedged forever. Host tests
could never have found this; it lives entirely in the twelve lines that are not
covered by them. The QEMU run found it in the only way it can be found, by
running.

**Undefined behaviour that a passing test walked straight past.** The moving
average computed its rounding constant as `1 << (shift - 1)` before checking
whether `shift` was zero. With a window of one, `shift - 1` is 4294967295 and
the shift is undefined — but the value was then never used, so the
window-of-one unit test passed. UBSan aborted on it. That is the precise shape
of bug that survives a green test suite and changes behaviour on the next
compiler.

**A formatter that produced a plausible wrong answer.** `ef_append_i32` shared
the string-append truncation rule, so a number that did not fit was truncated
mid-digits: `VAL 999999` could be emitted as `VAL 99`. A visibly missing value
is recoverable; a wrong one that looks valid is not. It is now all-or-nothing.

## And one lesson about the tests themselves

Three of the first-draft QEMU assertions were not testing the firmware at all.
They were testing the host, and CI caught each of them by disagreeing with a
laptop:

- **`sovr=0`** — the count of missed scheduler deadlines. A 1 ms task running
  late under an emulator says something about how busy the runner was. The
  counter existing and being reported is the feature; a fixed value is not.
- **`stackfree=3636`** — a real measurement, and perfectly deterministic for a
  given binary, but a different GCC lays out frames differently. Pinning it
  turns a toolchain upgrade into a test failure.
- **"the queue must overflow"** — QEMU feeds the UART with no baud rate, so
  whether a 1,000-byte burst outruns the 1 kHz drain depends on emulation
  speed. On one runner it overflowed; on another every byte got through. Both
  are legitimate.

The last one was the instructive one, because the fix was not to loosen the
assertion but to find the invariant that is actually true either way:

```
rx + drop == bytes sent
```

Every byte the ISR accepted was either parsed or counted as dropped. Nothing
vanishes. That holds whether or not the queue overflows, it fails loudly if the
accounting is ever wrong, and it says something about the firmware rather than
about the machine it ran on. The deterministic overflow tests live in
`tests/test_ringbuf.c`, where a queue can be filled exactly on purpose.

Likewise, the streaming case now waits until the firmware has produced twenty
reports rather than sleeping three seconds and hoping. A test that fails for
reasons unrelated to the code trains you to ignore it, which is worse than not
having it.

## Layout

```
core/    portable C, host-tested: ringbuf, parser, filter, sched, fmt
bsp/     hardware: uart, gpio, systick, startup, linker script
app/     the super-loop wiring core to bsp
tests/   host unit tests (native cc, run in CI, ~60 lines of harness)
qemu/    integration runner and committed output fixtures
tools/   footprint reporting
docs/    how-it-works.md — ISR safety, fixed point, scheduler, memory map
```

## Make targets

| | |
|---|---|
| `make test` | host unit tests |
| `make test-san` | the same tests under ASan + UBSan |
| `make firmware` | link the ARM image, print the footprint |
| `make size` | footprint table only |
| `make no-heap` | prove the image cannot allocate |
| `make cppcheck` | static analysis |
| `make qemu` | QEMU integration tests |
| `make all` | everything, in the order CI runs it |

`make test` needs only a C compiler. The ARM half needs `arm-none-eabi-gcc`
(`brew install arm-none-eabi-gcc`, or `apt-get install gcc-arm-none-eabi`) and
`qemu-system-arm`.

## What is not here

- **No real board.** Nothing has been flashed to silicon. If that changes, the
  evidence will be a separate, clearly-labelled section — not a quiet edit to
  the numbers above.
- **No real sensor.** The streaming waveform is synthetic and deterministic, so
  that the tests reproduce exactly.
- **No claim about timing.** The scheduler's periods are what the code asks
  for; the `sovr` counter reports when a deadline was missed, and under an
  emulator that number reflects the host's load as much as the firmware's.
- **No RTOS, no MPU, no power management, no DMA.** The scheduler is a task
  table and a tick comparison, and the README does not imply otherwise.
