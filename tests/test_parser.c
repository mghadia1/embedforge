/* Parser: the happy path is the easy half. The cases that matter are partial
 * input, garbage, and the overlong-line boundary. */
#include <string.h>

#include "ef_test.h"
#include "parser.h"

static ef_cmd run(const char *s)
{
    ef_parser p;
    ef_parser_init(&p);
    return ef_parser_push_str(&p, s);
}

EF_TEST(nullary_commands)
{
    EF_ASSERT_EQ_I(run("PING\n").kind, EF_CMD_PING);
    EF_ASSERT_EQ_I(run("GET\n").kind, EF_CMD_GET);
    EF_ASSERT_EQ_I(run("STATS\n").kind, EF_CMD_STATS);
    EF_ASSERT_EQ_I(run("QUIT\n").kind, EF_CMD_QUIT);
}

EF_TEST(case_insensitive)
{
    EF_ASSERT_EQ_I(run("ping\n").kind, EF_CMD_PING);
    EF_ASSERT_EQ_I(run("PiNg\n").kind, EF_CMD_PING);
    EF_ASSERT_EQ_I(run("stream on\n").kind, EF_CMD_STREAM);
    EF_ASSERT_EQ_I(run("Stream Off\n").arg, 0);
}

EF_TEST(whitespace_tolerated)
{
    EF_ASSERT_EQ_I(run("   PING   \n").kind, EF_CMD_PING);
    EF_ASSERT_EQ_I(run("\tGET\t\n").kind, EF_CMD_GET);
    EF_ASSERT_EQ_I(run("SET    42\n").arg, 42);
    EF_ASSERT_EQ_I(run("  SET\t-7  \n").arg, -7);
}

EF_TEST(crlf_same_as_lf)
{
    EF_ASSERT_EQ_I(run("PING\r\n").kind, EF_CMD_PING);
    EF_ASSERT_EQ_I(run("SE\rT 5\n").arg, 5); /* CR ignored mid-token too */
}

EF_TEST(blank_lines_produce_nothing)
{
    ef_parser p;
    ef_cmd out;
    ef_parser_init(&p);
    EF_ASSERT(!ef_parser_push(&p, '\n', &out));
    EF_ASSERT(!ef_parser_push(&p, '\r', &out));
    EF_ASSERT(!ef_parser_push(&p, '\n', &out));
    EF_ASSERT_EQ_I(ef_parser_push_str(&p, "   \n").kind, EF_CMD_NONE);
    EF_ASSERT_EQ_I(p.lines, 0);
    EF_ASSERT_EQ_I(p.errors, 0);
}

EF_TEST(integer_arguments)
{
    EF_ASSERT_EQ_I(run("SET 0\n").arg, 0);
    EF_ASSERT_EQ_I(run("SET +5\n").arg, 5);
    EF_ASSERT_EQ_I(run("FEED -1234\n").arg, -1234);
    EF_ASSERT_EQ_I(run("SET 2147483647\n").arg, 2147483647);
    EF_ASSERT_EQ_I(run("SET 2147483647\n").kind, EF_CMD_SET);
    /* INT32_MIN parses; the magnitude never passes through a signed negate. */
    EF_ASSERT_EQ_I(run("SET -2147483648\n").arg, INT32_MIN);
    EF_ASSERT_EQ_I(run("SET -2147483648\n").kind, EF_CMD_SET);
}

EF_TEST(integer_overflow_rejected)
{
    EF_ASSERT_EQ_I(run("SET 2147483648\n").kind, EF_CMD_ERR_ARG);
    EF_ASSERT_EQ_I(run("SET -2147483649\n").kind, EF_CMD_ERR_ARG);
    EF_ASSERT_EQ_I(run("SET 99999999999\n").kind, EF_CMD_ERR_ARG);
    EF_ASSERT_EQ_I(run("FEED 4294967296\n").kind, EF_CMD_ERR_ARG);
}

EF_TEST(bad_arguments_rejected)
{
    EF_ASSERT_EQ_I(run("SET\n").kind, EF_CMD_ERR_ARG);       /* missing     */
    EF_ASSERT_EQ_I(run("SET abc\n").kind, EF_CMD_ERR_ARG);   /* not a number*/
    EF_ASSERT_EQ_I(run("SET 12x\n").kind, EF_CMD_ERR_ARG);   /* trailing    */
    EF_ASSERT_EQ_I(run("SET -\n").kind, EF_CMD_ERR_ARG);     /* sign only   */
    EF_ASSERT_EQ_I(run("SET 1 2\n").kind, EF_CMD_ERR_ARG);   /* extra token */
    EF_ASSERT_EQ_I(run("STREAM\n").kind, EF_CMD_ERR_ARG);
    EF_ASSERT_EQ_I(run("STREAM MAYBE\n").kind, EF_CMD_ERR_ARG);
    /* A nullary command with an argument is an error, not a shrug. */
    EF_ASSERT_EQ_I(run("PING 1\n").kind, EF_CMD_ERR_ARG);
    EF_ASSERT_EQ_I(run("GET now\n").kind, EF_CMD_ERR_ARG);
}

EF_TEST(unknown_command)
{
    EF_ASSERT_EQ_I(run("NOPE\n").kind, EF_CMD_ERR_UNKNOWN);
    EF_ASSERT_EQ_I(run("PIN\n").kind, EF_CMD_ERR_UNKNOWN);   /* prefix     */
    EF_ASSERT_EQ_I(run("PINGG\n").kind, EF_CMD_ERR_UNKNOWN); /* extension  */
    EF_ASSERT_EQ_I(run("\x01\x02\x03\n").kind, EF_CMD_ERR_UNKNOWN);
}

/* A command delivered one byte at a time -- i.e. one UART interrupt per
 * character -- must parse identically to one delivered whole. */
EF_TEST(byte_at_a_time_equals_whole_line)
{
    ef_parser p;
    ef_cmd out;
    const char *s = "SET 1234\n";
    size_t i;
    ef_parser_init(&p);
    for (i = 0u; s[i] != '\0'; i++) {
        bool ready = ef_parser_push(&p, (uint8_t)s[i], &out);
        EF_ASSERT_EQ_I(ready, (s[i] == '\n'));
    }
    EF_ASSERT_EQ_I(out.kind, EF_CMD_SET);
    EF_ASSERT_EQ_I(out.arg, 1234);
}

EF_TEST(multiple_commands_one_burst)
{
    ef_parser p;
    ef_cmd out;
    const char *s = "PING\nGET\nSET 9\nBAD\n";
    ef_cmd seen[8];
    size_t n = 0u, i;

    ef_parser_init(&p);
    for (i = 0u; s[i] != '\0'; i++) {
        if (ef_parser_push(&p, (uint8_t)s[i], &out)) {
            seen[n++] = out;
        }
    }
    EF_ASSERT_EQ_I(n, 4);
    EF_ASSERT_EQ_I(seen[0].kind, EF_CMD_PING);
    EF_ASSERT_EQ_I(seen[1].kind, EF_CMD_GET);
    EF_ASSERT_EQ_I(seen[2].kind, EF_CMD_SET);
    EF_ASSERT_EQ_I(seen[2].arg, 9);
    EF_ASSERT_EQ_I(seen[3].kind, EF_CMD_ERR_UNKNOWN);
    EF_ASSERT_EQ_I(p.lines, 3);
    EF_ASSERT_EQ_I(p.errors, 1);
}

/* The length boundary, exactly. LINE_MAX characters is fine; one more is
 * ERR_TOO_LONG and yields exactly one result, not one per excess byte. */
EF_TEST(line_length_boundary)
{
    char buf[EF_PARSER_LINE_MAX + 64];
    size_t pad;

    /* "SET " + digits, padded to exactly EF_PARSER_LINE_MAX characters. */
    memset(buf, ' ', sizeof buf);
    memcpy(buf, "SET 1", 5);
    buf[EF_PARSER_LINE_MAX] = '\n';
    buf[EF_PARSER_LINE_MAX + 1] = '\0';
    EF_ASSERT_EQ_I(run(buf).kind, EF_CMD_SET);
    EF_ASSERT_EQ_I(run(buf).arg, 1);

    /* One character over. */
    buf[EF_PARSER_LINE_MAX] = ' ';
    buf[EF_PARSER_LINE_MAX + 1] = '\n';
    buf[EF_PARSER_LINE_MAX + 2] = '\0';
    EF_ASSERT_EQ_I(run(buf).kind, EF_CMD_ERR_TOO_LONG);

    /* Far over: still exactly one error result for the whole line. */
    for (pad = 0u; pad < sizeof buf - 2u; pad++) {
        buf[pad] = 'A';
    }
    buf[sizeof buf - 2u] = '\n';
    buf[sizeof buf - 1u] = '\0';
    {
        ef_parser p;
        ef_cmd out;
        size_t i, results = 0u;
        ef_parser_init(&p);
        for (i = 0u; buf[i] != '\0'; i++) {
            if (ef_parser_push(&p, (uint8_t)buf[i], &out)) {
                results++;
                EF_ASSERT_EQ_I(out.kind, EF_CMD_ERR_TOO_LONG);
            }
        }
        EF_ASSERT_EQ_I(results, 1);
        EF_ASSERT_EQ_I(p.errors, 1);
    }
}

/* An overlong line must not be truncated into a valid command: the prefix
 * "PING" is discarded along with the rest, not accepted. */
EF_TEST(overlong_line_cannot_smuggle_a_command)
{
    char buf[EF_PARSER_LINE_MAX + 16];
    memset(buf, 'X', sizeof buf);
    memcpy(buf, "PING", 4);
    buf[sizeof buf - 2u] = '\n';
    buf[sizeof buf - 1u] = '\0';
    EF_ASSERT_EQ_I(run(buf).kind, EF_CMD_ERR_TOO_LONG);
}

/* After an overlong line, the parser must recover cleanly on the next one. */
EF_TEST(recovers_after_overlong_line)
{
    ef_parser p;
    ef_cmd out;
    ef_cmd seen[4];
    size_t n = 0u, i;
    char buf[EF_PARSER_LINE_MAX * 3];

    memset(buf, 'Z', sizeof buf);
    buf[sizeof buf - 1u] = '\0';

    ef_parser_init(&p);
    for (i = 0u; buf[i] != '\0'; i++) {
        if (ef_parser_push(&p, (uint8_t)buf[i], &out)) { seen[n++] = out; }
    }
    EF_ASSERT_EQ_I(n, 0);
    EF_ASSERT_EQ_I(ef_parser_state_get(&p), EF_PS_DISCARD);

    if (ef_parser_push(&p, '\n', &out)) { seen[n++] = out; }
    EF_ASSERT_EQ_I(ef_parser_state_get(&p), EF_PS_LINE);

    {
        const char *next = "PING\n";
        for (i = 0u; next[i] != '\0'; i++) {
            if (ef_parser_push(&p, (uint8_t)next[i], &out)) { seen[n++] = out; }
        }
    }
    EF_ASSERT_EQ_I(n, 2);
    EF_ASSERT_EQ_I(seen[0].kind, EF_CMD_ERR_TOO_LONG);
    EF_ASSERT_EQ_I(seen[1].kind, EF_CMD_PING);
}

EF_TEST(embedded_nul_is_not_a_terminator)
{
    ef_parser p;
    ef_cmd out;
    const uint8_t bytes[] = { 'P', 'I', 0x00, 'N', 'G', '\n' };
    size_t i;
    bool ready = false;

    ef_parser_init(&p);
    for (i = 0u; i < sizeof bytes; i++) {
        ready = ef_parser_push(&p, bytes[i], &out);
    }
    /* The NUL is just another byte that makes the token not "PING". */
    EF_ASSERT(ready);
    EF_ASSERT_EQ_I(out.kind, EF_CMD_ERR_UNKNOWN);
}

EF_TEST(high_bytes_rejected_not_crashed)
{
    ef_parser p;
    ef_cmd out;
    unsigned v;
    ef_parser_init(&p);
    for (v = 0x80u; v <= 0xFFu; v++) {
        (void)ef_parser_push(&p, (uint8_t)v, &out);
        if (ef_parser_push(&p, '\n', &out)) {
            EF_ASSERT(out.kind == EF_CMD_ERR_UNKNOWN ||
                      out.kind == EF_CMD_ERR_TOO_LONG);
        }
    }
}

EF_TEST(cmd_names_are_all_distinct_strings)
{
    EF_ASSERT_STR_EQ(ef_cmd_name(EF_CMD_PING), "PING");
    EF_ASSERT_STR_EQ(ef_cmd_name(EF_CMD_ERR_ARG), "ERR_ARG");
    EF_ASSERT_STR_EQ(ef_cmd_name(EF_CMD_ERR_TOO_LONG), "ERR_TOO_LONG");
}

int main(void)
{
    EF_RUN(nullary_commands);
    EF_RUN(case_insensitive);
    EF_RUN(whitespace_tolerated);
    EF_RUN(crlf_same_as_lf);
    EF_RUN(blank_lines_produce_nothing);
    EF_RUN(integer_arguments);
    EF_RUN(integer_overflow_rejected);
    EF_RUN(bad_arguments_rejected);
    EF_RUN(unknown_command);
    EF_RUN(byte_at_a_time_equals_whole_line);
    EF_RUN(multiple_commands_one_burst);
    EF_RUN(line_length_boundary);
    EF_RUN(overlong_line_cannot_smuggle_a_command);
    EF_RUN(recovers_after_overlong_line);
    EF_RUN(embedded_nul_is_not_a_terminator);
    EF_RUN(high_bytes_rejected_not_crashed);
    EF_RUN(cmd_names_are_all_distinct_strings);
    return ef_test_report("parser");
}
