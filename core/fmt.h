/* fmt.h — the tiny bit of formatting the firmware needs, without libc.
 *
 * The firmware links -nostdlib, so there is no printf and no heap. These are
 * the only text-producing routines in the build. All of them write into a
 * caller-supplied buffer and never write past `cap`.
 */
#ifndef EF_FMT_H
#define EF_FMT_H

#include <stddef.h>
#include <stdint.h>

/* Length of a NUL-terminated string. */
size_t ef_strlen(const char *s);

/* Write `v` in decimal into dst[0..cap-1] as a NUL-terminated string.
 * Returns the number of characters written, excluding the NUL, or 0 if the
 * buffer is too small (in which case dst[0] is set to NUL when cap > 0).
 * INT32_MIN is handled. */
size_t ef_fmt_i32(char *dst, size_t cap, int32_t v);

/* Append `src` to the NUL-terminated string in dst, never exceeding cap
 * (including the NUL). Returns the new length. Truncates rather than
 * overflowing. */
size_t ef_append(char *dst, size_t cap, const char *src);

/* Append `v` in decimal. Unlike ef_append, this is all-or-nothing: if the full
 * number does not fit, nothing is appended and the existing string is left
 * unchanged. A half-written number is a plausible-looking wrong reading, which
 * is worse than a visibly missing one. Returns the resulting length. */
size_t ef_append_i32(char *dst, size_t cap, int32_t v);

#endif /* EF_FMT_H */
