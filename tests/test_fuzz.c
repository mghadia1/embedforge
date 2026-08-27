/* Property / fuzz tests over random byte streams.
 *
 * The parser is the only part of this firmware that touches attacker-shaped
 * input: whatever arrives on the wire lands in its line buffer. The claim it
 * has to earn is not "it parses commands" but "no byte sequence makes it read
 * or write outside that buffer, and no byte sequence makes it accept something
 * it should reject".
 *
 * The generator is a seeded xorshift32, so every run on every machine feeds
 * exactly the same bytes. A CI failure here is reproducible from the seed
 * printed in the failure message, which is the only kind of fuzz failure worth
 * having in a build gate. Run under -fsanitize=address,undefined in CI
 * (`make test-san`), where an out-of-bounds access aborts rather than passing
 * silently.
 */
#include <string.h>

#include "ef_test.h"
#include "parser.h"
#include "ringbuf.h"

#define GUARD_LEN 64u
#define GUARD_BYTE 0x5Au

/* The parser wrapped in guard bands. ASan catches an overflow into unrelated
 * memory; these bands catch an overflow that happens to land inside the same
 * allocation, which ASan cannot see. */
typedef struct {
    uint8_t   front[GUARD_LEN];
    ef_parser p;
    uint8_t   back[GUARD_LEN];
} guarded_parser;

static void guarded_init(guarded_parser *g)
{
    memset(g->front, GUARD_BYTE, sizeof g->front);
    memset(g->back, GUARD_BYTE, sizeof g->back);
    ef_parser_init(&g->p);
}

static void guards_intact(const guarded_parser *g)
{
    unsigned i;
    for (i = 0u; i < GUARD_LEN; i++) {
        if (g->front[i] != GUARD_BYTE) {
            EF_FAIL("front guard byte %u overwritten with 0x%02X", i,
                    g->front[i]);
            return;
        }
        if (g->back[i] != GUARD_BYTE) {
            EF_FAIL("back guard byte %u overwritten with 0x%02X", i,
                    g->back[i]);
            return;
        }
    }
    ef_test_checks++;
}

static bool kind_is_valid(ef_cmd_kind k)
{
    switch (k) {
    case EF_CMD_NONE:
    case EF_CMD_PING:
    case EF_CMD_GET:
    case EF_CMD_SET:
    case EF_CMD_FEED:
    case EF_CMD_STREAM:
    case EF_CMD_STATS:
    case EF_CMD_QUIT:
    case EF_CMD_ERR_UNKNOWN:
    case EF_CMD_ERR_ARG:
    case EF_CMD_ERR_TOO_LONG:
        return true;
    default:
        return false;
    }
}

/* Uniform random bytes: mostly unprintable, almost never a newline. This is the
 * "line noise on the UART" case. */
EF_TEST(uniform_random_bytes_never_break_invariants)
{
    guarded_parser g;
    ef_rng rng = { 0xA5A5A5A5u };
    unsigned long i;
    unsigned long results = 0u;

    guarded_init(&g);
    for (i = 0ul; i < 2000000ul; i++) {
        ef_cmd out;
        uint8_t b = (uint8_t)(ef_rng_next(&rng) & 0xFFu);

        if (ef_parser_push(&g.p, b, &out)) {
            results++;
            if (!kind_is_valid(out.kind)) {
                EF_FAIL("invalid kind %d at byte %lu", (int)out.kind, i);
                return;
            }
        }
        /* The buffer index must never leave its range, whatever arrives. */
        if (g.p.len > EF_PARSER_LINE_MAX) {
            EF_FAIL("len %u exceeds LINE_MAX at byte %lu", g.p.len, i);
            return;
        }
        if (g.p.state != EF_PS_LINE && g.p.state != EF_PS_DISCARD) {
            EF_FAIL("state %d is not a valid state at byte %lu",
                    (int)g.p.state, i);
            return;
        }
    }
    guards_intact(&g);

    /* Every result was either a command or an error, and the counters agree. */
    EF_ASSERT_EQ_I(g.p.lines + g.p.errors, results);
    EF_ASSERT_EQ_I(g.p.bytes, 2000000ul);
    /* Line noise should essentially never form a valid command. Anything else
     * would mean the parser is far too permissive. */
    EF_ASSERT_EQ_I(g.p.lines, 0);
    EF_ASSERT(results > 0ul); /* newlines did occur, so lines were terminated */
}

/* A byte alphabet skewed towards the command language: newlines, spaces,
 * digits and the letters of the keywords. This is where a mis-parse is
 * actually reachable, so it gets its own generator. */
EF_TEST(command_shaped_noise_never_mis_parses)
{
    static const char alphabet[] = "\n\n\n  SETFEDGTREAMONQUIPSTAV0123456789-+\r\t";
    guarded_parser g;
    ef_rng rng = { 0x0D15EA5Eu };
    unsigned long i;
    unsigned long accepted = 0u;

    guarded_init(&g);
    for (i = 0ul; i < 2000000ul; i++) {
        ef_cmd out;
        uint8_t b = (uint8_t)alphabet[ef_rng_next(&rng) % (sizeof alphabet - 1u)];

        if (ef_parser_push(&g.p, b, &out)) {
            if (!kind_is_valid(out.kind)) {
                EF_FAIL("invalid kind %d at byte %lu", (int)out.kind, i);
                return;
            }
            if (out.kind < EF_CMD_ERR_UNKNOWN && out.kind != EF_CMD_NONE) {
                accepted++;
                /* Every accepted command's argument must be inside the range
                 * its own grammar allows. */
                if (out.kind == EF_CMD_STREAM &&
                    (out.arg != 0 && out.arg != 1)) {
                    EF_FAIL("STREAM arg %d is neither ON nor OFF", (int)out.arg);
                    return;
                }
            }
        }
        if (g.p.len > EF_PARSER_LINE_MAX) {
            EF_FAIL("len %u exceeds LINE_MAX at byte %lu", g.p.len, i);
            return;
        }
    }
    guards_intact(&g);
    /* This alphabet does form real commands -- otherwise the test would be
     * proving nothing about the accept path. */
    EF_ASSERT(accepted > 0ul);
    EF_ASSERT_EQ_I(g.p.lines, accepted);
}

/* Long unterminated runs: the case that overflows a buffer if the length check
 * is wrong. Lines of every length from 0 to 4x LINE_MAX, each terminated. */
EF_TEST(every_line_length_is_safe)
{
    unsigned len;
    for (len = 0u; len <= EF_PARSER_LINE_MAX * 4u; len++) {
        guarded_parser g;
        ef_cmd out;
        unsigned i;
        unsigned results = 0u;

        guarded_init(&g);
        for (i = 0u; i < len; i++) {
            if (ef_parser_push(&g.p, (uint8_t)'A', &out)) { results++; }
            EF_ASSERT(g.p.len <= EF_PARSER_LINE_MAX);
        }
        if (ef_parser_push(&g.p, (uint8_t)'\n', &out)) {
            results++;
            EF_ASSERT(out.kind == (len == 0u ? EF_CMD_NONE
                     : (len <= EF_PARSER_LINE_MAX ? EF_CMD_ERR_UNKNOWN
                                                  : EF_CMD_ERR_TOO_LONG)));
        }
        EF_ASSERT_EQ_I(results, (len == 0u) ? 0u : 1u);
        guards_intact(&g);
    }
}

/* The real pipeline: random bytes into the ring buffer from a "producer",
 * drained by a "consumer" that feeds the parser -- with the drain rate varying
 * so the queue genuinely fills and drops. Nothing here may corrupt anything;
 * a full queue is allowed to lose bytes, and losing bytes may only ever cost a
 * command, never fabricate one. */
EF_TEST(ringbuf_to_parser_pipeline)
{
    uint8_t storage[64];
    ef_ringbuf rb;
    guarded_parser g;
    ef_rng rng = { 0xFEEDFACEu };
    unsigned long step;
    unsigned long pushed = 0u, popped = 0u;

    EF_ASSERT(ef_rb_init(&rb, storage, (uint16_t)sizeof storage));
    guarded_init(&g);

    for (step = 0ul; step < 200000ul; step++) {
        unsigned n_in  = ef_rng_next(&rng) % 80u;   /* burst can exceed capacity */
        unsigned n_out = ef_rng_next(&rng) % 40u;   /* drain is often too slow   */
        unsigned k;

        for (k = 0u; k < n_in; k++) {
            if (ef_rb_push(&rb, (uint8_t)(ef_rng_next(&rng) & 0xFFu))) {
                pushed++;
            }
        }
        for (k = 0u; k < n_out; k++) {
            uint8_t b;
            ef_cmd out;
            if (!ef_rb_pop(&rb, &b)) { break; }
            popped++;
            if (ef_parser_push(&g.p, b, &out)) {
                EF_ASSERT(kind_is_valid(out.kind));
            }
            EF_ASSERT(g.p.len <= EF_PARSER_LINE_MAX);
        }
        /* Conservation: everything accepted is either still queued or parsed. */
        EF_ASSERT_EQ_I(pushed - popped, ef_rb_count(&rb));
    }

    guards_intact(&g);
    EF_ASSERT_EQ_I(g.p.bytes, popped);
    /* The queue really did overflow, so the drop path was exercised rather
     * than merely present. */
    EF_ASSERT(ef_rb_drops(&rb) > 0u);
}

int main(void)
{
    EF_RUN(uniform_random_bytes_never_break_invariants);
    EF_RUN(command_shaped_noise_never_mis_parses);
    EF_RUN(every_line_length_is_safe);
    EF_RUN(ringbuf_to_parser_pipeline);
    return ef_test_report("fuzz");
}
