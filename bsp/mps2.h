/* mps2.h — register map for the ARM MPS2 AN385 (Cortex-M3) target.
 *
 * Only the peripherals this firmware actually drives are declared. Everything
 * here comes from the ARM CMSDK / MPS2 AN385 documentation and matches what
 * QEMU's `mps2-an385` machine models.
 */
#ifndef EF_MPS2_H
#define EF_MPS2_H

#include <stdint.h>

/* AN385 runs the Cortex-M3 at 25 MHz. */
#define MPS2_SYSCLK_HZ 25000000u

/* ---- CMSDK APB UART ---- */

typedef struct {
    volatile uint32_t DATA;      /* 0x00 RW  tx on write, rx on read       */
    volatile uint32_t STATE;     /* 0x04 RW  see UART_STATE_*              */
    volatile uint32_t CTRL;      /* 0x08 RW  see UART_CTRL_*               */
    volatile uint32_t INTSTATUS; /* 0x0C RW  read: pending; write 1: clear */
    volatile uint32_t BAUDDIV;   /* 0x10 RW  sysclk / baud, minimum 16     */
} cmsdk_uart_t;

#define UART0 ((cmsdk_uart_t *)0x40004000u)

#define UART_STATE_TX_FULL  (1u << 0)
#define UART_STATE_RX_FULL  (1u << 1)
#define UART_STATE_TX_OVER  (1u << 2)
#define UART_STATE_RX_OVER  (1u << 3)

#define UART_CTRL_TX_EN     (1u << 0)
#define UART_CTRL_RX_EN     (1u << 1)
#define UART_CTRL_TX_INT_EN (1u << 2)
#define UART_CTRL_RX_INT_EN (1u << 3)

#define UART_INT_TX         (1u << 0)
#define UART_INT_RX         (1u << 1)

/* ---- CMSDK APB GPIO ---- */

typedef struct {
    volatile uint32_t DATA;        /* 0x00 */
    volatile uint32_t DATAOUT;     /* 0x04 */
    volatile uint32_t reserved0[2];
    volatile uint32_t OUTENSET;    /* 0x10 */
    volatile uint32_t OUTENCLR;    /* 0x14 */
} cmsdk_gpio_t;

#define GPIO0 ((cmsdk_gpio_t *)0x40010000u)

/* ---- ARMv7-M core peripherals ---- */

typedef struct {
    volatile uint32_t CTRL;   /* 0xE000E010 */
    volatile uint32_t LOAD;
    volatile uint32_t VAL;
    volatile uint32_t CALIB;
} systick_t;

#define SYSTICK ((systick_t *)0xE000E010u)

#define SYSTICK_CTRL_ENABLE    (1u << 0)
#define SYSTICK_CTRL_TICKINT   (1u << 1)
#define SYSTICK_CTRL_CLKSOURCE (1u << 2) /* processor clock, not reference */

/* NVIC interrupt set-enable, 32 IRQs per register. */
#define NVIC_ISER ((volatile uint32_t *)0xE000E100u)

/* AN385 wires UART0's receive interrupt to IRQ 0 and its transmit interrupt to
 * IRQ 1. Only receive is used: transmit is polled, because the firmware is
 * never in a hurry to send and a TX ISR would add a second concurrency edge
 * for no benefit. */
#define IRQ_UART0_RX 0u
#define IRQ_UART0_TX 1u

static inline void nvic_enable_irq(uint32_t irq)
{
    NVIC_ISER[irq >> 5u] = (uint32_t)1u << (irq & 31u);
}

#endif /* EF_MPS2_H */
