#include "bsp.h"
#include "bsp_internal.h"
#include "mps2.h"

void bsp_init(void)
{
    bsp_gpio_init();
    bsp_uart_init();
    /* SysTick last: once it is running, exceptions can fire, and nothing
     * should take an interrupt before the peripherals it touches are up. */
    bsp_systick_init();
}

void bsp_shutdown(void)
{
    /* ARM semihosting SYS_EXIT (0x18) with ADP_Stopped_ApplicationExit.
     * QEMU implements this when started with -semihosting-config, which is how
     * the integration test ends on the firmware's own decision rather than on
     * a wall-clock timeout -- a timeout would make the test both slower and
     * flakier.
     *
     * With no debugger and no semihosting host, BKPT escalates to a HardFault,
     * which halts in HardFault_Handler. Either way the system stops. */
    __asm__ volatile(
        "mov r0, %[op]  \n\t"   /* 0x18    = SYS_EXIT                     */
        "mov r1, %[arg] \n\t"   /* 0x20026 = ADP_Stopped_ApplicationExit  */
        "bkpt 0xAB      \n\t"
        :
        : [op] "r" (0x18u), [arg] "r" (0x20026u)
        : "r0", "r1", "memory");

    for (;;) {
    }
}
