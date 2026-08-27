#include "bsp.h"
#include "mps2.h"

/* The registered sink and its context. Written once at init, read in the ISR.
 * `volatile` here is about the compiler, not about ordering: bsp_init stores
 * these before enabling the interrupt, so there is no race to order. */
static void (*rx_fn)(uint8_t byte, void *ctx) = 0;
static void *rx_ctx = 0;
static volatile uint32_t rx_overruns = 0u;

/* Registering the sink is also what arms the receive interrupt, and that
 * ordering is deliberate.
 *
 * An earlier version enabled RX_INT_EN and the NVIC line inside bsp_uart_init()
 * and left the sink to be attached later by main(). That is a real bug, and the
 * QEMU integration test is what exposed it: between the two calls the ISR was
 * live with a null sink, so every byte that arrived in that window was read out
 * of the hardware and thrown away. With input already queued on the wire at
 * reset -- exactly what a scripted test does, and exactly what a host that
 * starts talking immediately does -- that window swallowed the whole opening
 * burst, and the firmware looked like it had a dead receive path.
 *
 * Making the interrupt impossible to enable before a consumer exists removes
 * the window rather than shrinking it. */
void bsp_uart_set_rx_handler(void (*fn)(uint8_t byte, void *ctx), void *ctx)
{
    rx_fn  = fn;
    rx_ctx = ctx;

    if (fn == 0) {
        return;
    }

    /* Four steps, and the order of every one of them is load-bearing. This
     * sequence took a QEMU run to get right; the notes are here so the next
     * reader does not have to repeat it.
     *
     * 1. Clear any stale interrupt the block powered up with.
     *
     * 2. Enable RX_EN and RX_INT_EN in a SINGLE store. The UART decides
     *    whether an incoming byte raises an interrupt at the instant it
     *    latches that byte, from RX_INT_EN as it stands right then, and never
     *    revisits the decision. Set RX_EN first and RX_INT_EN a few
     *    instructions later, and a byte arriving in between lands in the
     *    holding register having raised nothing. The register is now full so
     *    no further byte is accepted, and nothing will ever read it because no
     *    interrupt is coming: the receive path is wedged permanently. Not one
     *    lost character -- every character, silently, and only when the peer
     *    happens to talk during those few instructions.
     *
     * 3. Read DATA once. This empties the holding register, and it is what
     *    tells the UART it may accept input again. QEMU's model in particular
     *    only re-offers bytes to its character backend on a DATA read, so
     *    without this the emulated firmware receives nothing at all.
     *
     * 4. Enable the NVIC line last. Steps 2 and 3 cannot lose a byte while it
     *    is masked: an interrupt raised in between simply latches as pending
     *    and is taken the moment the line is enabled. Doing this first would
     *    instead open a window where the ISR could race the flush in step 3
     *    for the same byte. */
    UART0->INTSTATUS = UART_INT_RX | UART_INT_TX;
    UART0->CTRL = UART_CTRL_TX_EN | UART_CTRL_RX_EN | UART_CTRL_RX_INT_EN;
    (void)UART0->DATA;
    nvic_enable_irq(IRQ_UART0_RX);
}

void bsp_uart_init(void)
{
    /* BAUDDIV is sysclk/baud and the block requires at least 16. QEMU does not
     * time the line, but a wrong divisor is still a real-hardware bug, so it is
     * computed rather than hardcoded. */
    UART0->BAUDDIV = MPS2_SYSCLK_HZ / 115200u;

    /* Transmit only. The receiver stays switched off entirely until a sink is
     * registered -- not merely un-interrupted, but off -- for the reason
     * spelled out in bsp_uart_set_rx_handler(). */
    UART0->CTRL = UART_CTRL_TX_EN;
}

void bsp_uart_putc(char c)
{
    while ((UART0->STATE & UART_STATE_TX_FULL) != 0u) {
        /* Polled transmit: the main loop is allowed to block here. Doing this
         * from an ISR would let a slow line stall the receive path. */
    }
    UART0->DATA = (uint32_t)(uint8_t)c;
}

void bsp_uart_write(const char *s)
{
    while (*s != '\0') {
        bsp_uart_putc(*s++);
    }
}

uint32_t bsp_uart_rx_overruns(void)
{
    return rx_overruns;
}

/* UART0 receive ISR -- the producer end of the SPSC ring buffer.
 *
 * Everything expensive is deliberately absent: no parsing, no formatting, no
 * transmit. It reads one byte and hands it to the sink, which is a single
 * lock-free push. That keeps the interrupt short enough that it cannot
 * meaningfully delay the next one, and it means the ISR shares exactly one
 * data structure with the main loop -- the one whose ordering is specified. */
void UART0RX_Handler(void)
{
    const uint32_t state = UART0->STATE;

    /* Acknowledge the interrupt BEFORE consuming the byte, never after.
     *
     * Reading DATA frees the holding register, which lets the UART latch the
     * next byte immediately -- and that byte raises its own interrupt. A
     * write-1-to-clear issued after the read acknowledges the byte just taken
     * AND wipes the fresh interrupt belonging to the byte that arrived in
     * between, which nothing will ever raise again. The receive path then
     * delivers exactly one byte and wedges forever.
     *
     * This is the ordinary shape of the bug, not an emulator quirk: any
     * peripheral that can re-arm between the read and the acknowledge has it.
     * Acknowledging first is safe in the other direction -- if a byte lands
     * after the clear, its interrupt is raised after the clear and survives. */
    UART0->INTSTATUS = UART_INT_RX;

    const uint8_t byte = (uint8_t)UART0->DATA; /* reading DATA clears RX_FULL */

    if ((state & UART_STATE_RX_OVER) != 0u) {
        /* The hardware itself lost a byte before we got here. Clear the sticky
         * flag and count it: this is upstream of the ring buffer, so it is not
         * a ring-buffer drop and must not be reported as one. */
        UART0->STATE = UART_STATE_RX_OVER;
        rx_overruns++;
    }

    if (rx_fn != 0) {
        rx_fn(byte, rx_ctx);
    } else {
        rx_overruns++;
    }
}
