#!/usr/bin/env python3
"""Print the firmware's flash and RAM footprint from arm-none-eabi-size -A.

`size` without -A folds the reserved stack region into .bss, which makes the
headline number look four times worse than the firmware's actual global
footprint. This prints the per-section breakdown instead, and the totals it
reports are the ones README.md quotes.
"""
import subprocess
import sys

FLASH_BUDGET = 128 * 1024   # must match MEMORY in bsp/linker.ld
RAM_BUDGET = 32 * 1024

DESCRIPTIONS = [
    (".isr_vector", "vector table"),
    (".text", "code and read-only data"),
    (".data", "initialised globals (in RAM, image in flash)"),
    (".bss", "zeroed globals"),
    (".stack", "reserved stack region"),
]


def main(argv):
    elf = argv[1] if len(argv) > 1 else "build/firmware.elf"
    size_tool = argv[2] if len(argv) > 2 else "arm-none-eabi-size"

    out = subprocess.run([size_tool, "-A", elf], capture_output=True,
                         text=True, check=True).stdout

    sizes = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith("."):
            try:
                sizes[parts[0]] = int(parts[1])
            except ValueError:
                pass

    print("== footprint ==")
    for name, what in DESCRIPTIONS:
        print("  %-12s %7d B   %s" % (name, sizes.get(name, 0), what))

    flash = sizes.get(".isr_vector", 0) + sizes.get(".text", 0) + sizes.get(".data", 0)
    ram = sizes.get(".data", 0) + sizes.get(".bss", 0) + sizes.get(".stack", 0)

    print("  " + "-" * 24)
    print("  %-12s %7d B   of %6d  (%.2f%%)"
          % ("FLASH", flash, FLASH_BUDGET, flash * 100.0 / FLASH_BUDGET))
    print("  %-12s %7d B   of %6d  (%.2f%%)"
          % ("RAM", ram, RAM_BUDGET, ram * 100.0 / RAM_BUDGET))
    print()
    print("  Budgets are enforced by bsp/linker.ld: exceeding either fails the link,")
    print("  so these numbers are a property of the artifact, not a description of it.")

    if flash > FLASH_BUDGET or ram > RAM_BUDGET:
        print("\nFAIL: over budget")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
