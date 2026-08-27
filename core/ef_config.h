/* ef_config.h — compile-time sizing for the portable core.
 *
 * Every buffer in core/ is sized from this file. Nothing in core/ allocates,
 * so these constants are the complete memory budget of the algorithmic layer.
 */
#ifndef EF_CONFIG_H
#define EF_CONFIG_H

/* Ring buffer capacity used by the application. Must be a power of two.
 * 256 bytes holds ~8 maximum-length command lines, so the main loop can fall
 * behind by 8 lines before a byte is dropped. */
#define EF_RB_APP_CAPACITY 256u

/* Longest accepted command line, excluding the terminator. Anything longer is
 * discarded to the next newline and reported as ERR TOOLONG. */
#define EF_PARSER_LINE_MAX 32u

/* Largest moving-average window. Bounds the filter's static footprint and,
 * with EF_FILTER_SAMPLE_MAX, bounds the accumulator (see filter.h). */
#define EF_FILTER_MAX_WINDOW 64u

/* Samples are clamped to +/- this before entering the accumulator.
 * 2^20 * 64 windows = 2^26, comfortably inside int32_t. */
#define EF_FILTER_SAMPLE_MAX 1048576L

/* Maximum entries in a scheduler task table. */
#define EF_SCHED_MAX_TASKS 8u

#endif /* EF_CONFIG_H */
