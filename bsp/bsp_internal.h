/* Init entry points shared between the BSP translation units. Not part of the
 * application-facing interface in bsp.h. */
#ifndef EF_BSP_INTERNAL_H
#define EF_BSP_INTERNAL_H

#include <stdint.h>

void bsp_uart_init(void);
void bsp_systick_init(void);
void bsp_gpio_init(void);

/* Value written over the unused stack region before main() runs. */
#define EF_STACK_PAINT 0xA5A5A5A5u

#endif /* EF_BSP_INTERNAL_H */
