/* main.c — the wiring, and nothing but the wiring.
 *
 * This file is the seam between the two halves of the project: it owns the
 * static storage, hands the ISR its sink, builds the task table, and turns
 * parsed commands into replies. Every non-trivial decision it makes -- how a
 * byte gets from the ISR to the main loop, what a line means, what the sensor
 * reading is, when a task is due -- is delegated to core/, which is covered by
 * host tests. What is left here is short enough to read in one sitting, which
 * is the only assurance an untested file can offer.
 *
 * All storage is static. There is no allocation anywhere in the image.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp.h"
#include "ef_config.h"
#include "filter.h"
#include "fmt.h"
#include "parser.h"
#include "ringbuf.h"
#include "sched.h"

#define EF_VERSION "0.1.0"

/* Filter configuration. A 16-sample window at the 50 Hz sample task is a
 * 320 ms average; 3 agreeing samples debounces a further 60 ms. */
#define FILTER_WINDOW   16u
#define FILTER_DEBOUNCE 3u
#define FILTER_DEFAULT_THRESHOLD 500

/* Task periods, in 1 kHz ticks. */
#define PERIOD_COMMS      1u    /* 1 kHz: drain the UART queue        */
#define PERIOD_SAMPLE    20u    /* 50 Hz: take a sensor reading       */
#define PERIOD_REPORT   200u    /* 5 Hz: stream a reading, if enabled */
#define PERIOD_HEARTBEAT 500u   /* 2 Hz: blink                        */

/* Bytes drained from the ring buffer per comms task run. Bounded so that a
 * flood on the wire cannot keep this task running long enough to make the
 * others miss their deadlines -- backlog is left in the queue, which is what
 * the queue is for. At 115200 baud, 1 ms delivers about 12 bytes, so 64 is
 * ample headroom without being unbounded. */
#define COMMS_DRAIN_BUDGET 64u

/* ---- static state ---- */

static uint8_t    rx_storage[EF_RB_APP_CAPACITY];
static ef_ringbuf rx_queue;
static ef_parser  parser;
static ef_filter  filter;
static ef_sched   scheduler;

static bool     streaming     = false;
static uint32_t sample_phase  = 0u;
static uint32_t commands_run  = 0u;

/* ---- output helpers ---- */

static void reply(const char *s)
{
    bsp_uart_write(s);
    bsp_uart_write("\n");
}

/* Build a line in a fixed buffer and send it. 64 bytes is larger than the
 * longest reply the firmware can produce (the STATS line, below). */
#define LINE_CAP 96u

static void reply_value(void)
{
    char line[LINE_CAP];
    line[0] = '\0';
    (void)ef_append(line, LINE_CAP, "VAL ");
    (void)ef_append_i32(line, LINE_CAP, ef_filter_value(&filter));
    (void)ef_append(line, LINE_CAP,
                    ef_filter_state_get(&filter) == EF_FILTER_HIGH ? " HIGH"
                                                                   : " LOW");
    reply(line);
}

static void reply_stats(void)
{
    char line[LINE_CAP];
    line[0] = '\0';
    (void)ef_append(line, LINE_CAP, "STATS rx=");
    (void)ef_append_i32(line, LINE_CAP, (int32_t)parser.bytes);
    (void)ef_append(line, LINE_CAP, " cmd=");
    (void)ef_append_i32(line, LINE_CAP, (int32_t)parser.lines);
    (void)ef_append(line, LINE_CAP, " err=");
    (void)ef_append_i32(line, LINE_CAP, (int32_t)parser.errors);
    (void)ef_append(line, LINE_CAP, " drop=");
    (void)ef_append_i32(line, LINE_CAP, (int32_t)ef_rb_drops(&rx_queue));
    (void)ef_append(line, LINE_CAP, " ovr=");
    (void)ef_append_i32(line, LINE_CAP, (int32_t)bsp_uart_rx_overruns());
    (void)ef_append(line, LINE_CAP, " sovr=");
    (void)ef_append_i32(line, LINE_CAP, (int32_t)ef_sched_overruns(&scheduler));
    /* Bytes of the 4 KB stack region never touched since reset -- a real
     * high-water measurement taken from the paint pattern, not an estimate. */
    (void)ef_append(line, LINE_CAP, " stackfree=");
    (void)ef_append_i32(line, LINE_CAP, (int32_t)bsp_stack_free());
    reply(line);
}

static void reply_ok_i32(const char *what, int32_t v)
{
    char line[LINE_CAP];
    line[0] = '\0';
    (void)ef_append(line, LINE_CAP, "OK ");
    (void)ef_append(line, LINE_CAP, what);
    (void)ef_append(line, LINE_CAP, " ");
    (void)ef_append_i32(line, LINE_CAP, v);
    reply(line);
}

/* ---- the synthetic sensor ----
 *
 * There is no sensor on this target and none is claimed. This is a
 * deterministic triangle wave with a repeatable dither on top, so that the
 * streaming path produces values that move, change state, and reproduce
 * exactly on every run. It is a stand-in for an ADC read, nothing more. */
static int32_t sensor_read(void)
{
    const uint32_t period = 128u;
    const uint32_t p = sample_phase % period;
    int32_t base = (p < period / 2u) ? (int32_t)(p * 16u)
                                     : (int32_t)((period - p) * 16u);
    /* A small deterministic wobble, so the debounce has something to do. */
    int32_t dither = (int32_t)((sample_phase * 37u) % 41u) - 20;
    sample_phase++;
    return base + dither;
}

/* ---- ISR sink: the producer end of the SPSC queue ----
 *
 * This runs in interrupt context. It is one lock-free push and nothing else.
 * The return value is deliberately discarded: a full queue increments the
 * ring buffer's own drop counter, which STATS reports, and there is nothing
 * useful an ISR can do about it in the moment. */
static void rx_sink(uint8_t byte, void *ctx)
{
    (void)ef_rb_push((ef_ringbuf *)ctx, byte);
}

/* ---- command dispatch ---- */

static void handle(const ef_cmd *cmd)
{
    commands_run++;
    switch (cmd->kind) {
    case EF_CMD_PING:
        reply("PONG");
        break;
    case EF_CMD_GET:
        reply_value();
        break;
    case EF_CMD_SET:
        ef_filter_set_threshold(&filter, cmd->arg);
        reply_ok_i32("THRESH", cmd->arg);
        break;
    case EF_CMD_FEED:
        (void)ef_filter_push(&filter, cmd->arg);
        reply_ok_i32("FEED", cmd->arg);
        break;
    case EF_CMD_STREAM:
        streaming = (cmd->arg != 0);
        reply(streaming ? "OK STREAM ON" : "OK STREAM OFF");
        break;
    case EF_CMD_STATS:
        reply_stats();
        break;
    case EF_CMD_QUIT:
        reply("BYE");
        bsp_shutdown();
        break;
    case EF_CMD_ERR_UNKNOWN:
        reply("ERR UNKNOWN");
        break;
    case EF_CMD_ERR_ARG:
        reply("ERR ARG");
        break;
    case EF_CMD_ERR_TOO_LONG:
        reply("ERR TOOLONG");
        break;
    case EF_CMD_NONE:
    default:
        break;
    }
}

/* ---- tasks ---- */

/* Consumer end of the SPSC queue. Bounded drain: see COMMS_DRAIN_BUDGET. */
static void task_comms(void *ctx)
{
    uint16_t budget = COMMS_DRAIN_BUDGET;
    uint8_t  byte;
    ef_cmd   cmd;

    (void)ctx;
    while (budget-- > 0u && ef_rb_pop(&rx_queue, &byte)) {
        if (ef_parser_push(&parser, byte, &cmd)) {
            handle(&cmd);
        }
    }
}

static void task_sample(void *ctx)
{
    (void)ctx;
    if (streaming) {
        (void)ef_filter_push(&filter, sensor_read());
    }
}

static void task_report(void *ctx)
{
    (void)ctx;
    if (streaming) {
        reply_value();
    }
}

static void task_heartbeat(void *ctx)
{
    (void)ctx;
    bsp_led_toggle();
}

/* ---- entry point ---- */

int main(void)
{
    char line[LINE_CAP];

    bsp_init();

    /* The queue must exist before the ISR can reach it, so it is initialised
     * before the handler is registered -- and registering the handler is what
     * arms the receive interrupt, so there is no window in which bytes can
     * arrive with nowhere to go. See bsp/uart.c. */
    (void)ef_rb_init(&rx_queue, rx_storage, (uint16_t)EF_RB_APP_CAPACITY);
    ef_parser_init(&parser);
    (void)ef_filter_init(&filter, FILTER_WINDOW, FILTER_DEFAULT_THRESHOLD,
                         FILTER_DEBOUNCE);
    bsp_uart_set_rx_handler(rx_sink, &rx_queue);

    ef_sched_init(&scheduler);
    (void)ef_sched_add(&scheduler, "comms", task_comms, NULL, PERIOD_COMMS, 0u);
    (void)ef_sched_add(&scheduler, "sample", task_sample, NULL, PERIOD_SAMPLE, 0u);
    (void)ef_sched_add(&scheduler, "report", task_report, NULL, PERIOD_REPORT,
                       PERIOD_REPORT);
    (void)ef_sched_add(&scheduler, "beat", task_heartbeat, NULL,
                       PERIOD_HEARTBEAT, 0u);

    line[0] = '\0';
    (void)ef_append(line, LINE_CAP, "EMBEDFORGE " EF_VERSION " stack=");
    (void)ef_append_i32(line, LINE_CAP, (int32_t)bsp_stack_size());
    reply(line);
    reply("READY");

    /* The super-loop. No preemption, no task stacks, no scheduler state
     * machine: read the clock, run whatever is due, repeat. */
    for (;;) {
        (void)ef_sched_run(&scheduler, bsp_tick_get());
    }
}
