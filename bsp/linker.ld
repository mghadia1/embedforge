/* linker.ld — MPS2 AN385 (Cortex-M3) memory map.
 *
 * The board actually provides 4 MB of ZBT SRAM at each of 0x00000000 and
 * 0x20000000. The regions below are deliberately much smaller than that:
 * 128 KB of "flash" and 32 KB of RAM, which is a realistic budget for a part
 * this firmware would ship on.
 *
 * That makes the footprint claim enforceable rather than descriptive. If the
 * image grows past the budget the link FAILS -- it does not quietly spread
 * into memory the target might not have. The numbers in README.md come from
 * arm-none-eabi-size run on the image this script produces.
 */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 128K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

/* Reserved for the main stack. There is no second stack: the scheduler is
 * cooperative and every task runs to completion on this one, which is why a
 * single high-water measurement covers the whole system. */
_stack_size = 4K;

ENTRY(Reset_Handler)

SECTIONS
{
  /* The Cortex-M3 fetches the initial SP and the reset vector from the first
     two words at address 0, so the vector table must come first. */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  /* Unwind tables. Nothing here throws or unwinds, but the toolchain can still
     emit these; discarding them silently would hide a surprise, so they are
     placed and then show up in the size report if they ever appear. */
  .ARM.exidx :
  {
    __exidx_start = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end = .;
  } > FLASH

  /* Initialised globals live in RAM but their initial values ship in flash.
     _sidata is where Reset_Handler copies from. */
  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  /* The stack occupies the top of RAM. Declaring it as a real section rather
     than "whatever is left" means the link fails if globals and stack together
     exceed the 32 KB budget, instead of the two silently colliding at runtime
     the first time the call depth grows. */
  .stack (NOLOAD) :
  {
    . = ALIGN(8);
    _sstack = .;
    . = . + _stack_size;
    . = ALIGN(8);
    _estack = .;
  } > RAM

  /* There is no heap section, and that is the point: see README.md. No .heap,
     no _end/_sbrk symbols, nothing for a malloc implementation to grow into.
     A stray malloc would fail to link rather than fail at 3 a.m. */

  /DISCARD/ :
  {
    *(.ARM.attributes)
    *(.comment)
  }
}

ASSERT(_estack <= ORIGIN(RAM) + LENGTH(RAM),
       "RAM budget exceeded: globals plus stack do not fit")
