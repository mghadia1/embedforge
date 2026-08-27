#include <stdatomic.h>

#include "bsp.h"
#include "mps2.h"

/* The tick counter is written by the SysTick exception and read by the main
 * loop. A 32-bit aligned load is single-copy atomic on Cortex-M, so a plain
 * volatile would not tear -- but saying so with a relaxed atomic states the
 * intent in the type system rather than in a comment, and costs the same
 * instruction. Relaxed is sufficient: the counter carries no other data with
 * it, so there is nothing for it to order. */
static _Atomic uint32_t ticks = 0u;

void bsp_systick_init(void)
{
    /* 25 MHz / 1000 Hz = 25000 processor clocks per tick. LOAD is the reload
     * value, so it is one less than the period. */
    SYSTICK->LOAD = (MPS2_SYSCLK_HZ / BSP_TICK_HZ) - 1u;
    SYSTICK->VAL  = 0u;
    SYSTICK->CTRL = SYSTICK_CTRL_CLKSOURCE | SYSTICK_CTRL_TICKINT |
                    SYSTICK_CTRL_ENABLE;
}

uint32_t bsp_tick_get(void)
{
    return atomic_load_explicit(&ticks, memory_order_relaxed);
}

void SysTick_Handler(void)
{
    atomic_fetch_add_explicit(&ticks, 1u, memory_order_relaxed);
}
