#include "parser.h"

/* ---- character classification, ASCII only, no libc, no locale ---- */

static bool is_space(uint8_t c)
{
    return c == (uint8_t)' ' || c == (uint8_t)'\t';
}

static bool is_digit(uint8_t c)
{
    return c >= (uint8_t)'0' && c <= (uint8_t)'9';
}

static uint8_t to_upper(uint8_t c)
{
    return (c >= (uint8_t)'a' && c <= (uint8_t)'z') ? (uint8_t)(c - 32u) : c;
}

/* Case-insensitive compare of line[start..end) against a NUL-terminated
 * literal. `end` is exclusive and always <= p->len, so no read runs past the
 * buffer. */
static bool token_is(const char *line, uint8_t start, uint8_t end,
                     const char *literal)
{
    uint8_t i = start;
    size_t  j = 0u;

    while (i < end && literal[j] != '\0') {
        if (to_upper((uint8_t)line[i]) != to_upper((uint8_t)literal[j])) {
            return false;
        }
        i++;
        j++;
    }
    return (i == end) && (literal[j] == '\0');
}

/* Advance past spaces. */
static uint8_t skip_space(const char *line, uint8_t i, uint8_t len)
{
    while (i < len && is_space((uint8_t)line[i])) {
        i++;
    }
    return i;
}

/* Advance to the next space or end of line. */
static uint8_t token_end(const char *line, uint8_t i, uint8_t len)
{
    while (i < len && !is_space((uint8_t)line[i])) {
        i++;
    }
    return i;
}

/* Parse line[start..end) as a decimal int32 with optional sign.
 * Rejects an empty token, any non-digit, and anything outside int32 range --
 * the magnitude is accumulated in uint32_t and checked against the asymmetric
 * limits before conversion, so INT32_MIN parses and INT32_MAX+1 does not. */
static bool parse_i32(const char *line, uint8_t start, uint8_t end, int32_t *out)
{
    uint32_t mag = 0u;
    bool     neg = false;
    uint8_t  i   = start;
    uint32_t limit;

    if (i >= end) {
        return false;
    }
    if (line[i] == '-' || line[i] == '+') {
        neg = (line[i] == '-');
        i++;
    }
    if (i >= end) {
        return false;
    }

    limit = neg ? 2147483648u : 2147483647u;

    while (i < end) {
        uint8_t c = (uint8_t)line[i];
        if (!is_digit(c)) {
            return false;
        }
        if (mag > (limit - (uint32_t)(c - (uint8_t)'0')) / 10u) {
            return false; /* would exceed the range for this sign */
        }
        mag = mag * 10u + (uint32_t)(c - (uint8_t)'0');
        i++;
    }

    /* 0u - mag is the modular negation; assigning it to int32_t via the
     * two's-complement bit pattern is what gives INT32_MIN without ever
     * evaluating -(INT32_MIN). */
    *out = neg ? (int32_t)((uint32_t)0u - mag) : (int32_t)mag;
    return true;
}

/* ---- line dispatch ---- */

static void parse_line(const ef_parser *p, ef_cmd *out)
{
    const char *line = p->line;
    const uint8_t len = p->len;
    uint8_t s, e, as, ae;

    out->kind = EF_CMD_NONE;
    out->arg  = 0;

    s = skip_space(line, 0u, len);
    e = token_end(line, s, len);
    if (s == e) {
        return; /* blank or whitespace-only line: not an error, not a command */
    }

    /* Argument token, if any. */
    as = skip_space(line, e, len);
    ae = token_end(line, as, len);

    if (token_is(line, s, e, "PING") || token_is(line, s, e, "GET") ||
        token_is(line, s, e, "STATS") || token_is(line, s, e, "QUIT")) {
        /* Nullary commands: anything after the verb is an error, not noise. */
        if (as < len) {
            out->kind = EF_CMD_ERR_ARG;
            return;
        }
        out->kind = token_is(line, s, e, "PING")  ? EF_CMD_PING
                  : token_is(line, s, e, "GET")   ? EF_CMD_GET
                  : token_is(line, s, e, "STATS") ? EF_CMD_STATS
                                                  : EF_CMD_QUIT;
        return;
    }

    if (token_is(line, s, e, "SET")) {
        out->kind = parse_i32(line, as, ae, &out->arg) ? EF_CMD_SET
                                                       : EF_CMD_ERR_ARG;
    } else if (token_is(line, s, e, "FEED")) {
        out->kind = parse_i32(line, as, ae, &out->arg) ? EF_CMD_FEED
                                                       : EF_CMD_ERR_ARG;
    } else if (token_is(line, s, e, "STREAM")) {
        if (token_is(line, as, ae, "ON")) {
            out->kind = EF_CMD_STREAM;
            out->arg  = 1;
        } else if (token_is(line, as, ae, "OFF")) {
            out->kind = EF_CMD_STREAM;
            out->arg  = 0;
        } else {
            out->kind = EF_CMD_ERR_ARG;
        }
    } else {
        out->kind = EF_CMD_ERR_UNKNOWN;
    }

    /* Trailing junk after the argument is rejected rather than ignored: a
     * command that is not exactly what was meant should fail loudly. */
    if (out->kind != EF_CMD_NONE && out->kind != EF_CMD_ERR_UNKNOWN) {
        const uint8_t tail = skip_space(line, ae, len);
        if (tail < len) {
            out->kind = EF_CMD_ERR_ARG;
            out->arg  = 0;
        }
    }
}

/* ---- public API ---- */

void ef_parser_init(ef_parser *p)
{
    p->len    = 0u;
    p->state  = EF_PS_LINE;
    p->bytes  = 0u;
    p->lines  = 0u;
    p->errors = 0u;
}

bool ef_parser_push(ef_parser *p, uint8_t byte, ef_cmd *out)
{
    p->bytes++;

    if (byte == (uint8_t)'\r') {
        return false; /* CRLF and LF are the same line ending here */
    }

    if (byte != (uint8_t)'\n') {
        if (p->state == EF_PS_DISCARD) {
            return false; /* still throwing away an overlong line */
        }
        if (p->len >= (uint8_t)EF_PARSER_LINE_MAX) {
            /* One byte too many. Drop what we have -- deliberately, so a long
             * prefix cannot be truncated into a valid command -- and skip to
             * the terminator. */
            p->state = EF_PS_DISCARD;
            p->len   = 0u;
            return false;
        }
        p->line[p->len++] = (char)byte;
        return false;
    }

    /* byte == '\n': the line is complete. */
    if (p->state == EF_PS_DISCARD) {
        p->state  = EF_PS_LINE;
        p->len    = 0u;
        p->errors++;
        out->kind = EF_CMD_ERR_TOO_LONG;
        out->arg  = 0;
        return true;
    }

    parse_line(p, out);
    p->len = 0u;

    if (out->kind == EF_CMD_NONE) {
        return false;
    }
    if (out->kind >= EF_CMD_ERR_UNKNOWN) {
        p->errors++;
    } else {
        p->lines++;
    }
    return true;
}

ef_cmd ef_parser_push_str(ef_parser *p, const char *s)
{
    ef_cmd last = { EF_CMD_NONE, 0 };
    ef_cmd got;
    size_t i;

    for (i = 0u; s[i] != '\0'; i++) {
        if (ef_parser_push(p, (uint8_t)s[i], &got)) {
            last = got;
        }
    }
    return last;
}

ef_parser_state ef_parser_state_get(const ef_parser *p)
{
    return p->state;
}

const char *ef_cmd_name(ef_cmd_kind kind)
{
    switch (kind) {
    case EF_CMD_NONE:         return "NONE";
    case EF_CMD_PING:         return "PING";
    case EF_CMD_GET:          return "GET";
    case EF_CMD_SET:          return "SET";
    case EF_CMD_FEED:         return "FEED";
    case EF_CMD_STREAM:       return "STREAM";
    case EF_CMD_STATS:        return "STATS";
    case EF_CMD_QUIT:         return "QUIT";
    case EF_CMD_ERR_UNKNOWN:  return "ERR_UNKNOWN";
    case EF_CMD_ERR_ARG:      return "ERR_ARG";
    case EF_CMD_ERR_TOO_LONG: return "ERR_TOO_LONG";
    default:                  return "?";
    }
}
