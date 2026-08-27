/* fmt: the firmware has no printf, so these four functions produce every byte
 * it ever sends. Truncation behaviour is checked as carefully as the output,
 * because a formatter that overruns its buffer on a bare-metal target
 * overwrites whatever the linker put next to it. */
#include <stdio.h>
#include <string.h>

#include "ef_test.h"
#include "fmt.h"

EF_TEST(strlen_matches_libc)
{
    EF_ASSERT_EQ_I(ef_strlen(""), 0);
    EF_ASSERT_EQ_I(ef_strlen("a"), 1);
    EF_ASSERT_EQ_I(ef_strlen("hello"), 5);
    EF_ASSERT_EQ_I(ef_strlen("hello"), strlen("hello"));
}

EF_TEST(formats_integers)
{
    char b[16];
    EF_ASSERT_EQ_I(ef_fmt_i32(b, sizeof b, 0), 1);
    EF_ASSERT_STR_EQ(b, "0");
    (void)ef_fmt_i32(b, sizeof b, 7);       EF_ASSERT_STR_EQ(b, "7");
    (void)ef_fmt_i32(b, sizeof b, -7);      EF_ASSERT_STR_EQ(b, "-7");
    (void)ef_fmt_i32(b, sizeof b, 1000);    EF_ASSERT_STR_EQ(b, "1000");
    (void)ef_fmt_i32(b, sizeof b, -1000);   EF_ASSERT_STR_EQ(b, "-1000");
    EF_ASSERT_EQ_I(ef_fmt_i32(b, sizeof b, 2147483647), 10);
    EF_ASSERT_STR_EQ(b, "2147483647");
    /* INT32_MIN: the magnitude is accumulated unsigned, so this does not
     * depend on negating a value that has no positive counterpart. */
    EF_ASSERT_EQ_I(ef_fmt_i32(b, sizeof b, INT32_MIN), 11);
    EF_ASSERT_STR_EQ(b, "-2147483648");
}

/* Cross-check every formatted value against snprintf on the host. The firmware
 * cannot use snprintf; the test can, and that is the point of splitting the
 * code this way. */
EF_TEST(agrees_with_snprintf_on_random_values)
{
    ef_rng rng = { 0x1234567u };
    char mine[16], theirs[16];
    int i;

    for (i = 0; i < 200000; i++) {
        int32_t v = (int32_t)ef_rng_next(&rng);
        size_t n = ef_fmt_i32(mine, sizeof mine, v);
        int m = snprintf(theirs, sizeof theirs, "%d", (int)v);
        EF_ASSERT_EQ_I(n, m);
        if (strcmp(mine, theirs) != 0) {
            EF_FAIL("value %d: got \"%s\", want \"%s\"", (int)v, mine, theirs);
            return;
        }
    }
    /* Boundaries explicitly, not just whatever the RNG happened to hit. */
    {
        const int32_t edge[] = { 0, 1, -1, 9, 10, -9, -10, 99, 100,
                                 INT32_MAX, INT32_MIN, INT32_MAX - 1,
                                 INT32_MIN + 1 };
        size_t k;
        for (k = 0u; k < sizeof edge / sizeof edge[0]; k++) {
            (void)ef_fmt_i32(mine, sizeof mine, edge[k]);
            (void)snprintf(theirs, sizeof theirs, "%d", (int)edge[k]);
            EF_ASSERT_STR_EQ(mine, theirs);
        }
    }
}

/* Too small a buffer must fail cleanly -- return 0 and leave an empty string --
 * never write a partial number past the end. */
EF_TEST(refuses_a_buffer_that_is_too_small)
{
    char b[8];
    size_t cap;

    memset(b, 'X', sizeof b);
    EF_ASSERT_EQ_I(ef_fmt_i32(b, 0u, 12345), 0);   /* cap 0: writes nothing */
    EF_ASSERT_EQ_I(b[0], 'X');

    /* "12345" needs 6 bytes with the NUL. Anything less must be refused. */
    for (cap = 1u; cap < 6u; cap++) {
        memset(b, 'X', sizeof b);
        EF_ASSERT_EQ_I(ef_fmt_i32(b, cap, 12345), 0);
        EF_ASSERT_EQ_I(b[0], '\0');
        EF_ASSERT_EQ_I(b[cap], 'X');               /* nothing past cap */
    }
    memset(b, 'X', sizeof b);
    EF_ASSERT_EQ_I(ef_fmt_i32(b, 6u, 12345), 5);
    EF_ASSERT_STR_EQ(b, "12345");
    EF_ASSERT_EQ_I(b[6], 'X');
}

EF_TEST(append_builds_a_line)
{
    char b[32];
    b[0] = '\0';
    EF_ASSERT_EQ_I(ef_append(b, sizeof b, "VAL "), 4);
    EF_ASSERT_EQ_I(ef_append_i32(b, sizeof b, -42), 7);
    EF_ASSERT_EQ_I(ef_append(b, sizeof b, " HIGH"), 12);
    EF_ASSERT_STR_EQ(b, "VAL -42 HIGH");
}

/* Append truncates at the buffer edge rather than overflowing, and always
 * leaves a NUL-terminated string. */
EF_TEST(append_truncates_without_overflowing)
{
    char b[8];
    char guard[16];

    memset(guard, 'G', sizeof guard);
    b[0] = '\0';
    EF_ASSERT_EQ_I(ef_append(b, sizeof b, "abcdefghijklmnop"), 7);
    EF_ASSERT_STR_EQ(b, "abcdefg");
    EF_ASSERT_EQ_I(b[7], '\0');

    /* Appending to a full buffer is a no-op, not a corruption. */
    EF_ASSERT_EQ_I(ef_append(b, sizeof b, "more"), 7);
    EF_ASSERT_STR_EQ(b, "abcdefg");

    /* A number that will not fit is dropped whole, never half-written. */
    b[0] = '\0';
    (void)ef_append(b, sizeof b, "abcde");
    EF_ASSERT_EQ_I(ef_append_i32(b, sizeof b, 999999), 5);
    EF_ASSERT_STR_EQ(b, "abcde");

    {
        size_t i;
        for (i = 0u; i < sizeof guard; i++) {
            EF_ASSERT_EQ_I(guard[i], 'G');
        }
    }
}

int main(void)
{
    EF_RUN(strlen_matches_libc);
    EF_RUN(formats_integers);
    EF_RUN(agrees_with_snprintf_on_random_values);
    EF_RUN(refuses_a_buffer_that_is_too_small);
    EF_RUN(append_builds_a_line);
    EF_RUN(append_truncates_without_overflowing);
    return ef_test_report("fmt");
}
