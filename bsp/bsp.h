/* bsp.h — the entire hardware surface the application is allowed to touch.
 *
 * Everything below this line is target-specific and is NOT covered by the host
 * unit tests; everything in core/ is portable and IS. The interface is kept
 * this narrow on purpose -- five things (a tick, a byte out, a byte in, an LED,
 * a way to stop) -- so that almost all behaviour lives in the tested half.
 *
 * See docs/how-it-works.md for what that split does and does not prove.
 */
#ifndef EF_BSP_H
#define EF_BSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Called once from main before anything else: clocks, UART, SysTick, NVIC. */
void bsp_init(void);

/* ---- time ---- */

/* Free-running 1 kHz tick, wrapping every ~49.7 days. The scheduler's
 * comparisons are wraparound-safe, so the wrap is a non-event. */
uint32_t bsp_tick_get(void);

#define BSP_TICK_HZ 1000u

/* ---- UART ---- */

/* Register the byte sink for the receive ISR. `fn` runs in interrupt context:
 * it must be short, must not block, and must not call bsp_uart_write.
 * In this firmware it is a single ring-buffer push. */
void bsp_uart_set_rx_handler(void (*fn)(uint8_t byte, void *ctx), void *ctx);

/* Blocking, polled transmit. Called only from the main loop, never from an
 * ISR. */
void bsp_uart_write(const char *s);
void bsp_uart_putc(char c);

/* Bytes the ISR dropped because no handler was registered, plus hardware
 * receive-overrun events reported by the UART. Both mean input was lost
 * before the ring buffer ever saw it, which is a different failure from a
 * ring-buffer drop and is counted separately. */
uint32_t bsp_uart_rx_overruns(void);

/* ---- GPIO ---- */

/* Heartbeat LED. NOTE: the MPS2 GPIO block is not modelled by QEMU's
 * mps2-an385 machine, so this is write-only there and is NOT verified by the
 * QEMU integration test. It is included because a real board needs it, and it
 * is labelled here rather than quietly claimed. */
void bsp_led_set(bool on);
void bsp_led_toggle(void);

/* ---- stack ---- */

/* Bytes of the stack region never touched since reset, measured from the paint
 * pattern laid down before main(). This is a real high-water measurement, not
 * an estimate: see bsp/startup.c. */
uint32_t bsp_stack_free(void);

/* Total size of the stack region, from the linker script. */
uint32_t bsp_stack_size(void);

/* ---- shutdown ---- */

/* Stop the firmware. Under QEMU (and under a debugger) this is an ARM
 * semihosting SYS_EXIT, which is how the integration test terminates
 * deterministically instead of on a timeout. On a real board with no debugger
 * attached the BKPT escalates to a HardFault, which the fault handler turns
 * into a halt -- documented rather than pretended away. */
void bsp_shutdown(void);

#endif /* EF_BSP_H */
