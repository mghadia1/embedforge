/* parser.h — byte-at-a-time state machine over a newline-terminated command
 * protocol.
 *
 * Grammar (case-insensitive, leading/trailing/interior whitespace tolerated):
 *
 *     PING                 liveness check
 *     GET                  report the filtered value and debounced state
 *     SET <int32>          set the debounce threshold
 *     FEED <int32>         inject one sensor sample
 *     STREAM ON | OFF      enable/disable periodic reporting
 *     STATS                report counters
 *     QUIT                 shut down
 *
 * The parser is fed one byte at a time as it comes out of the ring buffer, so
 * a command split across two UART interrupts, or arriving one character per
 * second from a human with a terminal, is handled identically to a whole line
 * arriving at once. It holds one fixed-size line buffer and never allocates.
 *
 * Robustness contract, which the fuzz test enforces over random byte streams:
 *
 *   - It never reads or writes outside the line buffer, for any input.
 *   - Any byte value 0x00-0xFF is accepted as input. Bytes that cannot form a
 *     command produce an error result, never a crash and never a silent
 *     mis-parse.
 *   - A line longer than EF_PARSER_LINE_MAX does not overflow: the parser
 *     enters a discard state, throws away everything to the next newline, and
 *     reports EF_CMD_ERR_TOO_LONG once. It cannot be used to smuggle a command
 *     past the length check.
 *   - '\r' is ignored everywhere, so CRLF and LF terminators behave the same.
 *   - An empty or whitespace-only line yields no result at all.
 */
#ifndef EF_PARSER_H
#define EF_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ef_config.h"

typedef enum {
    EF_CMD_NONE = 0,
    EF_CMD_PING,
    EF_CMD_GET,
    EF_CMD_SET,          /* arg = new threshold        */
    EF_CMD_FEED,         /* arg = raw sample           */
    EF_CMD_STREAM,       /* arg = 1 for ON, 0 for OFF  */
    EF_CMD_STATS,
    EF_CMD_QUIT,
    EF_CMD_ERR_UNKNOWN,  /* first token is not a command          */
    EF_CMD_ERR_ARG,      /* command known, argument missing/bad   */
    EF_CMD_ERR_TOO_LONG  /* line exceeded EF_PARSER_LINE_MAX      */
} ef_cmd_kind;

typedef struct {
    ef_cmd_kind kind;
    int32_t     arg;
} ef_cmd;

typedef enum {
    EF_PS_LINE = 0,  /* accumulating a line          */
    EF_PS_DISCARD    /* overlong: skip to next '\n'  */
} ef_parser_state;

typedef struct {
    char            line[EF_PARSER_LINE_MAX];
    uint8_t         len;
    ef_parser_state state;

    uint32_t bytes;     /* bytes fed in                       */
    uint32_t lines;     /* lines that produced a command      */
    uint32_t errors;    /* lines that produced an EF_CMD_ERR_* */
} ef_parser;

void ef_parser_init(ef_parser *p);

/* Feed one byte. Returns true and fills *out when a line completed and
 * produced a result; returns false otherwise (mid-line, or a blank line). */
bool ef_parser_push(ef_parser *p, uint8_t byte, ef_cmd *out);

/* Convenience for tests: feed a NUL-terminated string, return the last result
 * produced (EF_CMD_NONE if none). */
ef_cmd ef_parser_push_str(ef_parser *p, const char *s);

ef_parser_state ef_parser_state_get(const ef_parser *p);

/* Human-readable name, for logs and test failure messages. */
const char *ef_cmd_name(ef_cmd_kind kind);

#endif /* EF_PARSER_H */
