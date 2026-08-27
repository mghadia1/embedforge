/* startup.c — reset vector, C runtime bring-up, and the exception table.
 *
 * Written in C rather than assembly. The only thing that genuinely needs
 * assembly on ARMv7-M is setting the initial stack pointer, and the core does
 * that in hardware: it loads SP from the first word of the vector table before
 * fetching the reset vector. Everything after that -- copying .data, zeroing
 * .bss, painting the stack -- is ordinary C that a reader can check, so it is
 * ordinary C.
 *
 * There is no `main()` prologue from a C library here: the firmware links
 * -nostdlib, so this file IS the C runtime.
 */
#include <stdint.h>

#include "bsp.h"
#include "bsp_internal.h"

/* Provided by linker.ld. Their addresses are the values; the types are
 * arbitrary. */
extern uint32_t _sidata;  /* .data initialisers, in flash            */
extern uint32_t _sdata;   /* .data start, in RAM                     */
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _sstack;  /* lowest address of the stack region      */
extern uint32_t _estack;  /* one past the highest: the initial SP    */

int main(void);

void Reset_Handler(void);
void Default_Handler(void);
void HardFault_Handler(void);
void SysTick_Handler(void);
void UART0RX_Handler(void);

/* Anything not overridden lands here. A silent infinite loop is a deliberate
 * choice over a silent return: an unexpected exception should stop the system
 * where a debugger can find it, not carry on with a corrupted state. */
void Default_Handler(void)
{
    for (;;) {
    }
}

/* Aliased to Default_Handler unless a driver defines them. */
#define WEAK_ALIAS __attribute__((weak, alias("Default_Handler")))

void NMI_Handler(void)        WEAK_ALIAS;
void MemManage_Handler(void)  WEAK_ALIAS;
void BusFault_Handler(void)   WEAK_ALIAS;
void UsageFault_Handler(void) WEAK_ALIAS;
void SVC_Handler(void)        WEAK_ALIAS;
void DebugMon_Handler(void)   WEAK_ALIAS;
void PendSV_Handler(void)     WEAK_ALIAS;

/* bsp_shutdown() issues a semihosting BKPT. With a debugger or QEMU's
 * semihosting enabled that never returns. Without one, the BKPT escalates to a
 * HardFault and arrives here -- so the documented "on real silicon this halts"
 * is actually implemented, not assumed. */
void HardFault_Handler(void)
{
    for (;;) {
    }
}

/* The vector table. Placed by the linker script at address 0, which is where
 * the Cortex-M3 reads SP and the reset vector from at power-on.
 *
 * The AN385 has 32 external interrupts. Entry 0 is UART0 receive and entry 1
 * is UART0 transmit; the rest are unused by this firmware and fault into
 * Default_Handler rather than running off the end of the table. */
__attribute__((used, section(".isr_vector")))
void (*const g_vector_table[16 + 32])(void) = {
    (void (*)(void))(&_estack), /* 0  initial stack pointer */
    Reset_Handler,              /* 1  reset                 */
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,                 /* reserved                 */
    SVC_Handler,
    DebugMon_Handler,
    0,                          /* reserved                 */
    PendSV_Handler,
    SysTick_Handler,            /* 15 SysTick               */

    UART0RX_Handler,            /* IRQ 0  UART0 receive     */
    Default_Handler,            /* IRQ 1  UART0 transmit    */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler
};

/* Fill the unused part of the stack region with a known pattern so the
 * high-water mark can be measured later by counting how much of it survived.
 *
 * The loop stops well short of the current frame -- painting over the frame we
 * are standing in would corrupt the return address. The 64-byte margin is
 * generous for a function this small; the cost of over-reserving is a slightly
 * pessimistic (that is, safe) free-stack figure. */
__attribute__((noinline))
static void paint_stack(void)
{
    uint32_t *p = &_sstack;
    /* Everything below the current frame, minus a margin, is fair game.
     * __builtin_frame_address(0) is the compiler's own answer for "where is my
     * frame", which beats taking the address of a dummy local and hoping the
     * optimiser leaves it where it was written. */
    const uintptr_t floor = (uintptr_t)__builtin_frame_address(0) - 64u;
    const uint32_t *const limit = (const uint32_t *)(floor & ~(uintptr_t)3u);

    /* cppcheck-suppress comparePointers
     * `_sstack` and the frame address are separate objects as far as the C
     * abstract machine is concerned, so comparing them is formally undefined
     * and the analyser is right to say so. It is also the only way to express
     * "walk the stack region", which is defined by the linker script and not
     * by any C declaration. This is the specific place where the firmware
     * steps outside the standard on purpose. */
    while (p < limit) {
        *p++ = EF_STACK_PAINT;
    }
}

uint32_t bsp_stack_size(void)
{
    return (uint32_t)((uintptr_t)&_estack - (uintptr_t)&_sstack);
}

uint32_t bsp_stack_free(void)
{
    /* Count intact paint words from the bottom of the region upward. The first
     * one that has been overwritten is the deepest the stack has ever reached.
     * This is a measurement of what actually happened since reset, not a
     * static estimate of what might. */
    const uint32_t *p = &_sstack;
    uint32_t untouched = 0u;

    /* cppcheck-suppress comparePointers
     * Same as paint_stack(): `_sstack` and `_estack` are linker symbols
     * bounding one region, not two C objects. */
    while (p < &_estack && *p == EF_STACK_PAINT) {
        untouched += 4u;
        p++;
    }
    return untouched;
}

void Reset_Handler(void)
{
    const uint32_t *src;
    uint32_t *dst;

    /* Copy initialised globals from their flash image into RAM. */
    src = &_sidata;
    /* cppcheck-suppress comparePointers
     * Linker-defined region bounds again -- see paint_stack(). */
    for (dst = &_sdata; dst < &_edata; dst++) {
        *dst = *src++;
    }

    /* Zero the uninitialised globals. Nothing in this firmware may read a
     * global before this line. */
    /* cppcheck-suppress comparePointers
     * Linker-defined region bounds again -- see paint_stack(). */
    for (dst = &_sbss; dst < &_ebss; dst++) {
        *dst = 0u;
    }

    paint_stack();

    (void)main();

    /* main() does not return in normal operation. If it ever does, stop here
     * rather than executing whatever follows in flash. */
    for (;;) {
    }
}
