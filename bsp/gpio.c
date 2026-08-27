#include "bsp.h"
#include "mps2.h"

/* MPS2 user LED 0. QEMU's mps2-an385 does not model the CMSDK GPIO block, so
 * these writes land in an unimplemented-device region there and have no
 * observable effect. Nothing in the QEMU integration test asserts on the LED,
 * and the README does not claim it works -- it is here because the same
 * firmware on real silicon needs it. */
#define LED0_BIT (1u << 0)

static bool led_state = false;

void bsp_gpio_init(void)
{
    GPIO0->OUTENSET = LED0_BIT;
    bsp_led_set(false);
}

void bsp_led_set(bool on)
{
    led_state = on;
    if (on) {
        GPIO0->DATAOUT |= LED0_BIT;
    } else {
        GPIO0->DATAOUT &= ~LED0_BIT;
    }
}

void bsp_led_toggle(void)
{
    bsp_led_set(!led_state);
}
