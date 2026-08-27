/* libc_shim.c — the two functions GCC is allowed to synthesise calls to.
 *
 * The firmware links -nostdlib. Nothing here calls memcpy or memset directly,
 * but the compiler may still emit calls to them for struct assignment or array
 * initialisation, so they have to exist. Providing them here rather than
 * linking newlib is what keeps the "no heap, no libc" claim literally true:
 * there is no libc in the image to accidentally pull malloc in with it.
 *
 * These are the obvious byte-at-a-time implementations. They are not on any
 * hot path -- the firmware's steady state is a few bytes of UART per second --
 * so a word-at-a-time version would be unjustified cleverness in the one file
 * where a subtle bug is hardest to see.
 */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0u) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0u) {
        *d++ = (uint8_t)value;
    }
    return dst;
}
