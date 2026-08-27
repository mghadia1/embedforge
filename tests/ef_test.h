/* ef_test.h — a ~60-line assert harness.
 *
 * Deliberately not a framework. The host tests need three things: run a named
 * case, report the first failure with file and line, exit non-zero. Anything
 * more would be a dependency to install in CI for no benefit.
 *
 * Usage:
 *     EF_TEST(name) { EF_ASSERT(cond); EF_ASSERT_EQ_I(a, b); }
 *     int main(void) { EF_RUN(name); return ef_test_report("suite"); }
 */
#ifndef EF_TEST_H
#define EF_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ef_test_failures = 0;
static int ef_test_cases    = 0;
static int ef_test_checks   = 0;
static const char *ef_test_current = "";

#define EF_TEST(name) static void name(void)

#define EF_RUN(name)                                                          \
    do {                                                                      \
        ef_test_current = #name;                                              \
        ef_test_cases++;                                                      \
        name();                                                               \
    } while (0)

#define EF_FAIL(fmt, ...)                                                     \
    do {                                                                      \
        ef_test_failures++;                                                   \
        fprintf(stderr, "FAIL %s (%s:%d): " fmt "\n", ef_test_current,        \
                __FILE__, __LINE__, __VA_ARGS__);                             \
    } while (0)

#define EF_ASSERT(cond)                                                       \
    do {                                                                      \
        ef_test_checks++;                                                     \
        if (!(cond)) {                                                        \
            EF_FAIL("%s", #cond);                                             \
        }                                                                     \
    } while (0)

/* Signed integer equality. Both sides are widened to long long so mixing
 * int32_t, uint8_t and enum operands does not need a cast at every call. */
#define EF_ASSERT_EQ_I(actual, expected)                                      \
    do {                                                                      \
        long long a_ = (long long)(actual);                                   \
        long long e_ = (long long)(expected);                                 \
        ef_test_checks++;                                                     \
        if (a_ != e_) {                                                       \
            EF_FAIL("%s == %s: got %lld, want %lld", #actual, #expected,      \
                    a_, e_);                                                  \
        }                                                                     \
    } while (0)

#define EF_ASSERT_STR_EQ(actual, expected)                                    \
    do {                                                                      \
        const char *a_ = (actual);                                            \
        const char *e_ = (expected);                                          \
        ef_test_checks++;                                                     \
        if (strcmp(a_, e_) != 0) {                                            \
            EF_FAIL("%s: got \"%s\", want \"%s\"", #actual, a_, e_);          \
        }                                                                     \
    } while (0)

static int ef_test_report(const char *suite)
{
    if (ef_test_failures == 0) {
        printf("ok   %-16s %d cases, %d checks\n", suite, ef_test_cases,
               ef_test_checks);
        return 0;
    }
    printf("FAIL %-16s %d failure(s) in %d cases\n", suite, ef_test_failures,
           ef_test_cases);
    return 1;
}

/* A seeded xorshift32, so "random" input in the fuzz tests is the same random
 * input on every machine and every run. A CI failure must be reproducible. */
typedef struct { uint32_t s; } ef_rng;

static inline uint32_t ef_rng_next(ef_rng *r)
{
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

#endif /* EF_TEST_H */
