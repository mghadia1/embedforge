#!/usr/bin/env python3
"""Boot the firmware under QEMU, drive its UART, and check what comes back.

This is the integration test. It is emulation, not silicon: QEMU models the
MPS2-AN385's Cortex-M3, its CMSDK UART and the SysTick timer, and the firmware
image it runs is byte-for-byte the one that would be flashed to a board. What
that proves is that the image boots, that the vector table and reset code are
right, that the UART driver and its interrupt work, and that the whole
core/ + bsp/ stack behaves end to end. What it does not prove is anything about
real timing, real electrical behaviour, or a real sensor -- see README.md.

Each case sends a scripted byte stream and checks the reply. Cases marked
"exact" are compared byte for byte against a committed fixture; the streaming
case cannot be, because how many periodic reports land in a given interval is a
timing property, so it asserts on structure instead.

Usage:
    python3 qemu/run_qemu.py --elf build/firmware.elf
    python3 qemu/run_qemu.py --elf build/firmware.elf --record   # regenerate
"""

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "cases")

MACHINE = "mps2-an385"
TIMEOUT_S = 30

# `-nographic` is not cosmetic here. It is the one serial configuration in which
# QEMU's chardev actually feeds guest input on this host: `-serial stdio` with
# `-display none`, `-serial tcp:...` and an explicit `-chardev socket` all
# transmit fine but never deliver a byte to the guest. Output is identical
# either way, so a run that looks healthy can still be receiving nothing --
# which is exactly how this took an afternoon to pin down the first time.
QEMU_ARGS = [
    "-M", MACHINE,
    "-nographic",
    # The firmware ends itself with a semihosting SYS_EXIT on QUIT, so the test
    # finishes on the guest's own decision rather than on a wall-clock timeout.
    # The timeout below is a backstop for a hang, not the normal path.
    "-semihosting-config", "enable=on,target=native",
]


class Case:
    def __init__(self, name, script, mode="exact", description=""):
        self.name = name
        self.script = script
        self.mode = mode
        self.description = description

    @property
    def expected_path(self):
        return os.path.join(CASES, self.name + ".expected")


# Every case ends in QUIT so the guest exits on its own.
CASES_LIST = [
    Case(
        "commands",
        "PING\n"
        "GET\n"
        "FEED 800\n"
        "GET\n"
        "SET 100\n"
        "GET\n"
        "STATS\n"
        "QUIT\n",
        description="every command, and the counters they report",
    ),
    Case(
        "filter",
        # A 16-sample moving average with a debounce of 3, threshold 500.
        # Three samples of 800 average to 800 and agree three times running,
        # so the third one is where the state flips to HIGH -- and not before.
        "SET 500\n"
        "FEED 800\n"
        "GET\n"
        "FEED 800\n"
        "GET\n"
        "FEED 800\n"
        "GET\n"
        # Now break it back down. The average has to fall below 500 first,
        # which takes more than one low sample because it is an average.
        "FEED 0\n"
        "GET\n"
        "FEED 0\n"
        "GET\n"
        "FEED 0\n"
        "GET\n"
        "FEED 0\n"
        "GET\n"
        "QUIT\n",
        description="fixed-point averaging and debounce, end to end on target",
    ),
    Case(
        "robustness",
        # Garbage, empty lines, CRLF, a line far over the length limit, and a
        # command split so the parser has to hold state across reads -- then a
        # valid command, to prove it recovered.
        "\n"
        "\r\n"
        "   \n"
        "NOSUCHCOMMAND\n"
        "SET\n"
        "SET notanumber\n"
        "SET 99999999999\n"
        "PING extra\n"
        "STREAM MAYBE\n"
        + "A" * 60 + "\n"
        "ping\r\n"
        "  \tGET\t \n"
        "STATS\n"
        "QUIT\n",
        description="malformed input is rejected, and the parser recovers",
    ),
    Case(
        "burst",
        "",  # the runner generates the flood
        mode="burst",
        description="a 1000-byte burst loses nothing silently",
    ),
    Case(
        "stream",
        "STREAM ON\n",   # the runner sends STREAM OFF / QUIT after a delay
        mode="stream",
        description="the periodic sample and report tasks under the scheduler",
    ),
]


def find_qemu():
    exe = shutil.which("qemu-system-arm")
    if exe is None:
        sys.exit(
            "qemu-system-arm not found. Install it to run the integration "
            "tests:\n"
            "      brew install qemu               (macOS)\n"
            "      apt-get install qemu-system-arm (Debian/Ubuntu)"
        )
    return exe


def run(qemu, elf, stdin_bytes, timeout=TIMEOUT_S):
    """Boot the image, feed it stdin_bytes, return (stdout, exit code)."""
    proc = subprocess.Popen(
        [qemu] + QEMU_ARGS + ["-kernel", elf],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    try:
        out, _ = proc.communicate(stdin_bytes, timeout=timeout)
        return out.decode("utf-8", "replace"), proc.returncode
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
        return out.decode("utf-8", "replace"), "timeout"


# How many periodic reports the streaming case waits for before stopping. The
# synthetic sensor sweeps a full triangle every 128 samples at 50 Hz, and a
# report is emitted every 10 samples, so 20 reports is about 1.5 full sweeps --
# comfortably enough for the debounced state to cross in both directions.
STREAM_REPORTS_WANTED = 20


def run_stream(qemu, elf):
    """Turn streaming on and wait for output, not for the clock.

    An earlier version slept a fixed three seconds. That silently assumes how
    fast QEMU emulates relative to real time, which is a property of the host:
    on a slower runner three wall-clock seconds is not enough guest time for
    the sensor to complete a sweep, and the run fails for a reason that has
    nothing to do with the firmware. Waiting until the firmware has actually
    produced N reports makes the case depend on guest progress instead, with
    the wall clock demoted to a hang backstop.
    """
    import select
    import time

    proc = subprocess.Popen(
        [qemu] + QEMU_ARGS + ["-kernel", elf],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    try:
        proc.stdin.write(b"STREAM ON\n")
        proc.stdin.flush()

        collected = b""
        deadline = time.time() + TIMEOUT_S / 2.0
        while time.time() < deadline:
            if collected.count(b"VAL ") >= STREAM_REPORTS_WANTED:
                break
            ready, _, _ = select.select([proc.stdout], [], [], 0.25)
            if ready:
                chunk = os.read(proc.stdout.fileno(), 4096)
                if not chunk:
                    break
                collected += chunk

        proc.stdin.write(b"STREAM OFF\nSTATS\nQUIT\n")
        proc.stdin.flush()
        rest, _ = proc.communicate(timeout=TIMEOUT_S)
        return (collected + rest).decode("utf-8", "replace"), proc.returncode
    except subprocess.TimeoutExpired:
        proc.kill()
        rest, _ = proc.communicate()
        return (collected + rest).decode("utf-8", "replace"), "timeout"


# The burst case sends 200 PING commands at once, then resynchronises and asks
# for STATS. These are the byte counts it expects to have been received by the
# time the firmware processes that STATS.
BURST_COMMANDS = 200
BURST_BYTES = BURST_COMMANDS * len("PING\n")          # 1000
RESYNC_AND_STATS = len("\nSTATS\n")                    # 7
BURST_TOTAL = BURST_BYTES + RESYNC_AND_STATS          # 1007


def run_burst(qemu, elf):
    """Fire a burst far larger than the receive queue, then check nothing was
    lost silently.

    QEMU hands the UART bytes as fast as the guest will take them, with no baud
    rate in the model. Whether that outruns the 1 kHz comms task depends on how
    fast the host is emulating relative to the guest's own clock -- so on one
    machine the 256-byte queue overflows and on another all 1000 bytes get
    through. Both are legitimate.

    So this case deliberately does NOT assert that an overflow happened. That
    would be a claim about the host, and the deterministic overflow tests live
    in tests/test_ringbuf.c where they belong. What it asserts instead is the
    invariant that holds either way:

        every byte the ISR accepted was either parsed or counted as dropped

    Nothing may vanish. If the queue did overflow, the loss is visible in
    drop= and the command straddling it must be rejected rather than
    mis-parsed; if it did not, every command must have been answered.
    """
    import time

    proc = subprocess.Popen(
        [qemu] + QEMU_ARGS + ["-kernel", elf],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.5)
        proc.stdin.write(b"PING\n" * BURST_COMMANDS)
        proc.stdin.flush()
        time.sleep(2.0)
        # A bare newline first, to resynchronise. If the queue overflowed, the
        # bytes lost were in the middle of a line, so the parser is left
        # holding a partial command; the next thing sent would be glued onto it
        # and rejected. Terminating that stump is how a real host recovers a
        # line protocol.
        proc.stdin.write(b"\nSTATS\nPING\nQUIT\n")
        proc.stdin.flush()
        out, _ = proc.communicate(timeout=TIMEOUT_S)
        return out.decode("utf-8", "replace"), proc.returncode
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
        return out.decode("utf-8", "replace"), "timeout"


def check_burst(out):
    problems = []
    lines = [l for l in out.split("\n") if l]
    pongs = [l for l in lines if l == "PONG"]

    stats = [l for l in lines if l.startswith("STATS ")]
    if not stats:
        problems.append("no STATS line: the firmware stopped responding")
        return problems

    fields = dict(re.findall(r"(\w+)=(\d+)", stats[-1]))
    missing = {"rx", "drop", "ovr"} - set(fields)
    if missing:
        problems.append("STATS is missing %s: %r" % (sorted(missing), stats[-1]))
        return problems

    rx = int(fields["rx"])
    drop = int(fields["drop"])

    # The invariant. Every byte sent up to and including the STATS newline was
    # either handed to the parser or refused by a full queue -- none went
    # missing in between.
    if rx + drop != BURST_TOTAL:
        problems.append(
            "conservation failed: rx=%d + drop=%d = %d, but %d bytes were sent "
            "before that STATS -- %d bytes are unaccounted for"
            % (rx, drop, rx + drop, BURST_TOTAL, BURST_TOTAL - (rx + drop))
        )

    # Whatever happened at the queue, the ISR itself must have kept up: a
    # hardware overrun would mean bytes lost before the queue ever saw them.
    if int(fields["ovr"]) != 0:
        problems.append(
            "hardware receive overrun: the ISR fell behind, %r" % stats[-1]
        )

    if drop > 0:
        # The queue overflowed. The loss must be visible in the command count,
        # and the command straddling it must be rejected, not mis-parsed.
        if len(pongs) >= BURST_COMMANDS + 1:
            problems.append(
                "drop=%d says bytes were lost, but all %d commands were "
                "answered" % (drop, len(pongs))
            )
        if "ERR UNKNOWN" not in lines:
            problems.append(
                "expected the command straddling the drop to be rejected"
            )
    else:
        # No overflow on this host: then nothing may have been lost at all.
        if len(pongs) != BURST_COMMANDS + 1:
            problems.append(
                "drop=0 says nothing was lost, but %d of %d commands were "
                "answered" % (len(pongs), BURST_COMMANDS + 1)
            )

    if not out.rstrip().endswith("BYE"):
        problems.append("firmware did not shut down cleanly after the burst")
    return problems


def normalise(text):
    """The UART emits bare LF; strip any CR a host layer added."""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def mask_volatile(text):
    """Blank the two STATS fields that are not portable, before comparing.

    Everything else in a STATS line is a pure function of the input -- byte
    counts, command counts, error counts, drops -- and is compared exactly.
    These two are not:

      sovr= is the scheduler overrun count. An overrun means a task ran a whole
            period late, which under an emulator depends on how busy the host
            was. It is real information (and the firmware reporting it is the
            feature working), but it is not a property of the firmware, so
            asserting a fixed value would make CI fail for reasons that have
            nothing to do with the code.

      stackfree= is the stack high-water mark. It is perfectly deterministic
            for a given binary, but a different GCC version lays out frames
            differently, and CI does not use the same compiler build as a
            laptop. Pinning the exact number would turn a toolchain upgrade
            into a test failure.

    Both are still checked, by check_stats_sanity() -- as ranges rather than as
    fixed values, which is what they actually support.
    """
    text = re.sub(r"sovr=\d+", "sovr=*", text)
    return re.sub(r"stackfree=\d+", "stackfree=*", text)


def check_stats_sanity(out):
    """Assert the masked fields are present and plausible."""
    problems = []
    for line in out.split("\n"):
        if not line.startswith("STATS "):
            continue
        m = re.search(r"stackfree=(\d+)", line)
        if not m:
            problems.append("STATS line has no stackfree field: %r" % line)
            continue
        free = int(m.group(1))
        # The stack region is 4096 bytes. Some of it must have been used, and
        # a lot of it must be left -- if either end of that is false the
        # measurement is broken or the firmware is close to overflowing.
        if not 0 < free < 4096:
            problems.append(
                "stack high-water mark of %d is outside the 4096-byte region "
                "-- the paint measurement is wrong" % free
            )
        elif free < 2048:
            problems.append(
                "only %d of 4096 stack bytes were never touched: the firmware "
                "is using more than half its stack" % free
            )
        if not re.search(r"sovr=\d+", line):
            problems.append("STATS line has no sovr field: %r" % line)
    return problems


def check_stream(out):
    """Structural assertions for the timing-dependent streaming case."""
    problems = []
    lines = [l for l in out.split("\n") if l]

    if "OK STREAM ON" not in lines:
        problems.append("streaming was never acknowledged")
    if "OK STREAM OFF" not in lines:
        problems.append("streaming was never turned off")

    vals = [l for l in lines if l.startswith("VAL ")]
    # The runner waits for this many reports rather than for a fixed time, so
    # falling short means the periodic task stopped, not that the host was
    # slow. A couple may arrive after the STREAM OFF is queued, hence the
    # upper slack.
    if len(vals) < STREAM_REPORTS_WANTED:
        problems.append(
            "expected at least %d periodic reports, got %d -- the report task "
            "stopped running" % (STREAM_REPORTS_WANTED, len(vals))
        )

    for l in vals:
        if not re.fullmatch(r"VAL -?\d+ (LOW|HIGH)", l):
            problems.append("malformed report line: %r" % l)
            break

    # The sensor is a triangle wave crossing the threshold, so the debounced
    # state must actually change during the run -- otherwise the filter is
    # wired up but doing nothing.
    states = {l.rsplit(" ", 1)[1] for l in vals}
    if states != {"LOW", "HIGH"}:
        problems.append(
            "expected the debounced state to change during the sweep, saw %s"
            % (sorted(states) or "nothing")
        )

    # Nothing may have been lost anywhere in the chain.
    stats = [l for l in lines if l.startswith("STATS ")]
    if not stats:
        problems.append("no STATS line")
    else:
        # No byte may be lost: not in the ring buffer (drop) and not in the
        # UART itself (ovr). Scheduler overruns are deliberately NOT asserted
        # here -- a 1 ms task running late under an emulator says something
        # about the host, not about the firmware, and the counter existing is
        # the feature.
        for field in ("drop=0", "ovr=0"):
            if field not in stats[-1]:
                problems.append("expected %s in %r" % (field, stats[-1]))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default="build/firmware.elf")
    ap.add_argument("--record", action="store_true",
                    help="overwrite the expected fixtures with this run")
    args = ap.parse_args()

    if not os.path.exists(args.elf):
        sys.exit("no firmware at %s -- run `make firmware` first" % args.elf)

    qemu = find_qemu()
    os.makedirs(CASES, exist_ok=True)

    print("== QEMU integration tests (%s, emulation) ==" % MACHINE)
    failures = 0

    for case in CASES_LIST:
        if case.mode == "stream":
            out, rc = run_stream(qemu, args.elf)
        elif case.mode == "burst":
            out, rc = run_burst(qemu, args.elf)
        else:
            out, rc = run(qemu, args.elf, case.script.encode())
        raw = normalise(out)
        out = mask_volatile(raw)

        if rc != 0:
            print("FAIL %-12s guest did not exit cleanly (%s)"
                  % (case.name, rc))
            print(out)
            failures += 1
            continue

        if case.mode in ("stream", "burst"):
            problems = (check_stream(raw) if case.mode == "stream"
                        else check_burst(raw))
            problems += check_stats_sanity(raw)
            if problems:
                print("FAIL %-12s %s" % (case.name, case.description))
                for p in problems:
                    print("       - %s" % p)
                print("     output was:\n%s" % raw)
                failures += 1
            elif case.mode == "stream":
                n = len([l for l in raw.split("\n") if l.startswith("VAL ")])
                print("ok   %-12s %s (%d periodic reports)"
                      % (case.name, case.description, n))
            else:
                stats = [l for l in raw.split("\n") if l.startswith("STATS ")]
                drops = int(re.search(r"drop=(\d+)", stats[-1]).group(1))
                how = ("queue overflowed, %d bytes dropped and accounted for"
                       % drops) if drops else "queue kept up, nothing dropped"
                print("ok   %-12s %s (%s)"
                      % (case.name, case.description, how))
            continue

        sanity = check_stats_sanity(raw)
        if sanity:
            print("FAIL %-12s %s" % (case.name, case.description))
            for p in sanity:
                print("       - %s" % p)
            failures += 1
            continue

        if args.record:
            with open(case.expected_path, "w") as f:
                f.write(out)
            print("rec  %-12s %s" % (case.name, case.description))
            continue

        if not os.path.exists(case.expected_path):
            print("FAIL %-12s no fixture; run with --record" % case.name)
            failures += 1
            continue

        with open(case.expected_path) as f:
            expected = f.read()

        if out != expected:
            print("FAIL %-12s %s" % (case.name, case.description))
            for line in difflib.unified_diff(
                expected.splitlines(True), out.splitlines(True),
                fromfile="expected", tofile="actual",
            ):
                print("     " + line.rstrip("\n"))
            failures += 1
        else:
            print("ok   %-12s %s" % (case.name, case.description))

    if args.record:
        print("\nfixtures written to %s -- read them before committing" % CASES)
        return 0

    if failures:
        print("\n%d QEMU case(s) failed" % failures)
        return 1
    print("\nall QEMU cases passed (emulated, not silicon)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
