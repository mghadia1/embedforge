#include "fmt.h"

size_t ef_strlen(const char *s)
{
    size_t n = 0u;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

size_t ef_fmt_i32(char *dst, size_t cap, int32_t v)
{
    char tmp[12]; /* -2147483648 is 11 chars */
    size_t n = 0u;
    size_t len;
    size_t i;
    int negative = (v < 0);

    /* Accumulate the magnitude in uint32_t. Negating INT32_MIN as a signed
     * value is undefined; the conversion to uint32_t is modular and defined,
     * and 0u - that value is exactly |v|. No 64-bit math, so no libgcc
     * helper call on Cortex-M3. */
    uint32_t mag = negative ? (uint32_t)0u - (uint32_t)v : (uint32_t)v;

    do {
        tmp[n++] = (char)('0' + (char)(mag % 10u));
        mag /= 10u;
    } while (mag != 0u);

    len = n + (negative ? 1u : 0u);
    if (cap < len + 1u) {
        if (cap > 0u) {
            dst[0] = '\0';
        }
        return 0u;
    }

    i = 0u;
    if (negative) {
        dst[i++] = '-';
    }
    while (n > 0u) {
        dst[i++] = tmp[--n];
    }
    dst[i] = '\0';
    return len;
}

size_t ef_append(char *dst, size_t cap, const char *src)
{
    size_t len = ef_strlen(dst);
    size_t i = 0u;

    if (cap == 0u) {
        return 0u;
    }
    while (src[i] != '\0' && len + 1u < cap) {
        dst[len++] = src[i++];
    }
    dst[len] = '\0';
    return len;
}

size_t ef_append_i32(char *dst, size_t cap, int32_t v)
{
    char   tmp[12];
    size_t len = ef_strlen(dst);
    size_t n   = ef_fmt_i32(tmp, sizeof tmp, v);

    /* All-or-nothing: only append if the whole number fits alongside the NUL.
     * Truncating digits would turn "VAL 999999" into "VAL 99" -- a wrong
     * reading that looks entirely valid downstream. */
    if (n == 0u || len + n + 1u > cap) {
        return len;
    }
    return ef_append(dst, cap, tmp);
}
