# EmbedForge — project spec

## Goal

Close the C / embedded gap in this portfolio with a project that is genuinely
verifiable: the algorithmic half proven by host unit tests in CI, the firmware
half actually booted and driven under emulation, and no claim on the README
that a reader cannot re-run in two minutes.

## Scope

A telemetry and command firmware for an ARM Cortex-M3 (`mps2-an385`, chosen
because QEMU models it faithfully and no board is needed):

- a lock-free single-producer / single-consumer ring buffer between the UART
  receive ISR and the main loop;
- a byte-at-a-time command-protocol state machine over a line protocol,
  tolerant of partial and malformed input;
- a fixed-point moving-average filter with debounce, no floating point;
- a cooperative super-loop scheduler driven by a 1 kHz SysTick.

## The design rule

`core/` is portable freestanding C with no hardware in it, host-tested in CI.
`bsp/` is the thin target layer. `app/` is wiring only. The intent is that
almost all behaviour lives in the tested half, and that the untested half is
small enough to read in one sitting.

## Definition of done

- [x] `core/` host unit tests pass in CI — ring buffer wrap/full/empty, parser
      states, filter values, scheduler timing
- [x] the same tests clean under AddressSanitizer and UndefinedBehaviorSanitizer
- [x] property/fuzz tests over seeded random byte streams; the parser never
      crashes or overflows, only rejects
- [x] `cppcheck` clean
- [x] `-Wall -Wextra -Werror` clean on host and target, plus `-Wconversion`,
      `-Wsign-conversion`, `-Wshadow`, `-Wcast-qual`
- [x] ARM firmware links inside an enforced flash/RAM budget
- [x] footprint reported from the actual image, reproducible from committed
      scripts
- [x] no dynamic allocation, verified in the source, the image and the linker
      script
- [x] QEMU integration tests pass on scripted input
- [x] `docs/how-it-works.md` written

## Résumé gate

`resume_eligible: no` until Mayank can explain, unaided:

1. why the SPSC ring buffer is safe with one producer in an ISR and one
   consumer in the main loop — the single-writer-per-index argument, and why
   `volatile` alone would not be enough;
2. one memory or timing constraint the design lives under — the accumulator
   bound, the 87 µs interrupt budget at 115200 baud, or the 49.7-day tick wrap;
3. one failure mode of the parser — what an overlong line does, or what happens
   to the command straddling a queue overflow.

## Explicitly out of scope

- Real silicon. Nothing has been flashed. If that changes it becomes a separate,
  clearly-labelled section with real artifacts, not an edit to existing numbers.
- A real sensor. The streaming waveform is synthetic and deterministic.
- Timing claims. The scheduler reports missed deadlines; under emulation that
  number reflects host load as much as firmware behaviour.
- RTOS, MPU, DMA, power management, bootloader, firmware update.

## Honesty rules

- No fabricated hardware runs or numbers. QEMU output and host tests are real
  and count; silicon claims require real-board artifacts.
- Footprint numbers come from an actual build and are reproducible via
  `make size`.
- Branch/PR isolation per change (`codex/embedforge-<topic>`); never push to
  main or rewrite history.
