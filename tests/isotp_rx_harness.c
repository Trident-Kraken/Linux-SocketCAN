#define _POSIX_C_SOURCE 200809L

#include "isotp.h"
#include "uds.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_RX_ID 0x7E0u
#define TEST_TX_ID 0x7E8u
#define TEST_MAX_SENT_FRAMES 600u

typedef struct {
    uint8_t data[ISOTP_MAX_LEN];
    uint16_t len;
    unsigned int count;
} capture_t;

static can_frame_t sent_frames[TEST_MAX_SENT_FRAMES];
static struct timespec sent_times[TEST_MAX_SENT_FRAMES];
static size_t sent_frame_count;
static unsigned int tx_done_count;
static int last_tx_status;
static int forced_send_status;
static capture_t capture;

static bool force_timer_expiry(isotp_ctx_t *ctx, isotp_timer_t timer_type);
static bool drive_stmin_timer(isotp_ctx_t *ctx, size_t target_frame_count);
static uint64_t elapsed_us(const struct timespec *start,
                           const struct timespec *end);
static bool sleep_milliseconds(long milliseconds);

int can_send(can_driver_t *driver, const can_frame_t *frame)
{
    (void)driver;
    if (forced_send_status != 0) {
        return forced_send_status;
    }
    if (frame == NULL || sent_frame_count >= sizeof(sent_frames) /
                            sizeof(sent_frames[0])) {
        return -EINVAL;
    }
    sent_frames[sent_frame_count++] = *frame;
    (void)clock_gettime(CLOCK_MONOTONIC, &sent_times[sent_frame_count - 1u]);
    return 0;
}

static void on_message(const uint8_t *data, uint16_t len, void *user)
{
    capture_t *result = user;

    if (result == NULL || data == NULL || len > ISOTP_MAX_LEN) {
        return;
    }
    (void)memcpy(result->data, data, len);
    result->len = len;
    result->count++;
}

static void on_tx_done(int status, void *user)
{
    (void)user;
    tx_done_count++;
    last_tx_status = status;
}

static bool check(bool condition, const char *message)
{
    if (!condition) {
        (void)fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

static void reset_capture(void)
{
    (void)memset(sent_frames, 0, sizeof(sent_frames));
    (void)memset(sent_times, 0, sizeof(sent_times));
    sent_frame_count = 0u;
    tx_done_count = 0u;
    last_tx_status = 0;
    forced_send_status = 0;
    (void)memset(&capture, 0, sizeof(capture));
}

static void init_frame(can_frame_t *frame)
{
    (void)memset(frame, 0, sizeof(*frame));
    frame->id = TEST_RX_ID;
    frame->dlc = CAN_CLASSIC_DLEN;
    (void)memset(frame->data, ISOTP_PADDING_BYTE, sizeof(frame->data));
}

static void init_context(isotp_ctx_t *ctx, can_driver_t *driver)
{
    isotp_config_t config = {0};
    int result;

    config.can = driver;
    config.rx_id = TEST_RX_ID;
    config.tx_id = TEST_TX_ID;
    config.on_message = on_message;
    config.on_tx_done = on_tx_done;
    config.user = &capture;
    result = isotp_init(ctx, &config);
    if (result < 0) {
        (void)fprintf(stderr, "isotp_init failed: %s\n", strerror(-result));
        exit(EXIT_FAILURE);
    }
}

static bool check_fc(void)
{
    static const uint8_t expected[CAN_CLASSIC_DLEN] = {
        0x30u, 0x00u, 0x00u, 0xAAu, 0xAAu, 0xAAu, 0xAAu, 0xAAu,
    };

    return check(sent_frame_count == 1u, "exactly one FC was sent") &&
           check(sent_frames[0].id == TEST_TX_ID, "FC uses response CAN ID") &&
           check(sent_frames[0].dlc == CAN_CLASSIC_DLEN, "FC uses DLC 8") &&
           check(memcmp(sent_frames[0].data, expected, sizeof(expected)) == 0,
                 "FC is CTS with BS=0 and STmin=0");
}

static bool send_ff(isotp_ctx_t *ctx, uint16_t total_len,
                    const uint8_t *payload)
{
    can_frame_t frame;
    int result;

    init_frame(&frame);
    frame.data[0] = (uint8_t)(0x10u | ((total_len >> 8) & 0x0Fu));
    frame.data[1] = (uint8_t)(total_len & 0xFFu);
    (void)memcpy(&frame.data[2], payload, 6u);
    result = isotp_on_frame(ctx, &frame);
    return check(result == 0, "FF is accepted");
}

static bool send_cf(isotp_ctx_t *ctx, uint8_t sn, const uint8_t *payload,
                    uint8_t payload_len)
{
    can_frame_t frame;
    int result;

    init_frame(&frame);
    frame.data[0] = (uint8_t)(0x20u | (sn & 0x0Fu));
    (void)memcpy(&frame.data[1], payload, payload_len);
    result = isotp_on_frame(ctx, &frame);
    return check(result == 0, "CF is processed");
}

static bool send_fc(isotp_ctx_t *ctx, uint8_t flow_status, uint8_t block_size,
                    uint8_t stmin)
{
    can_frame_t frame;
    int result;

    init_frame(&frame);
    frame.data[0] = (uint8_t)(0x30u | (flow_status & 0x0Fu));
    frame.data[1] = block_size;
    frame.data[2] = stmin;
    result = isotp_on_frame(ctx, &frame);
    return check(result == 0, "FC is processed");
}

static bool send_sf(isotp_ctx_t *ctx, const uint8_t *payload,
                    uint8_t payload_len)
{
    can_frame_t frame;
    int result;

    init_frame(&frame);
    frame.data[0] = payload_len;
    (void)memcpy(&frame.data[1], payload, payload_len);
    result = isotp_on_frame(ctx, &frame);
    return check(result == 0, "SF is processed");
}

static bool check_padding(const can_frame_t *frame, uint8_t first_unused)
{
    for (uint8_t index = first_unused; index < CAN_CLASSIC_DLEN; ++index) {
        if (frame->data[index] != ISOTP_PADDING_BYTE) {
            return false;
        }
    }
    return true;
}

static bool check_tx_frame(const can_frame_t *frame, uint8_t pci,
                           const uint8_t *payload, uint8_t payload_len)
{
    return check(frame != NULL && frame->id == TEST_TX_ID,
                 "TX frame uses response CAN ID") &&
           check(frame->dlc == CAN_CLASSIC_DLEN, "TX frame uses DLC 8") &&
           check(frame->data[0] == pci, "TX frame PCI is correct") &&
           check(memcmp(&frame->data[1], payload, payload_len) == 0,
                 "TX frame payload is correct") &&
           check(check_padding(frame, (uint8_t)(1u + payload_len)),
                 "TX frame padding is correct");
}

static bool scenario_basic(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    uint8_t payload[20];
    bool result = true;

    reset_capture();
    for (size_t index = 0; index < sizeof(payload); ++index) {
        payload[index] = (uint8_t)(index + 1u);
    }
    init_context(&ctx, &driver);
    result = send_ff(&ctx, sizeof(payload), payload) && check_fc();
    result = send_cf(&ctx, 1u, &payload[6], 7u) && result;
    result = send_cf(&ctx, 2u, &payload[13], 7u) && result;
    result = check(capture.count == 1u, "one complete message callback") &&
             result;
    result = check(capture.len == sizeof(payload), "20-byte payload length") &&
             result;
    result = check(memcmp(capture.data, payload, sizeof(payload)) == 0,
                   "20-byte payload contents") && result;
    isotp_deinit(&ctx);
    return result;
}

static bool scenario_tx_basic(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    const uint8_t one[] = {0x10u};
    const uint8_t seven[] = {1u, 2u, 3u, 4u, 5u, 6u, 7u};
    const uint8_t eight[] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    const uint8_t replacement[] = {0x99u};
    uint8_t too_long[ISOTP_MAX_LEN + 1u] = {0};
    bool result = true;

    reset_capture();
    init_context(&ctx, &driver);
    result = check(isotp_send(&ctx, one, sizeof(one)) == 0,
                   "one-byte SF is accepted") && result;
    result = check(sent_frame_count == 1u && tx_done_count == 1u &&
                       last_tx_status == 0,
                   "one-byte SF completes synchronously") && result;
    result = check_tx_frame(&sent_frames[0], 0x01u, one, sizeof(one)) &&
             result;
    isotp_deinit(&ctx);

    reset_capture();
    init_context(&ctx, &driver);
    result = check(isotp_send(&ctx, seven, sizeof(seven)) == 0,
                   "seven-byte SF is accepted") && result;
    result = check(sent_frame_count == 1u && tx_done_count == 1u &&
                       last_tx_status == 0,
                   "seven-byte SF completes synchronously") && result;
    result = check_tx_frame(&sent_frames[0], 0x07u, seven, sizeof(seven)) &&
             result;
    isotp_deinit(&ctx);

    reset_capture();
    init_context(&ctx, &driver);
    result = check(isotp_send(&ctx, eight, sizeof(eight)) == 0,
                   "eight-byte payload is accepted") && result;
    result = check(sent_frame_count == 1u && tx_done_count == 0u &&
                       isotp_tx_busy(&ctx),
                   "eight-byte payload starts with FF and waits for FC") &&
             result;
    result = check(sent_frames[0].data[0] == 0x10u &&
                       sent_frames[0].data[1] == 0x08u &&
                       memcmp(&sent_frames[0].data[2], eight, 6u) == 0,
                   "FF length and first six payload bytes are correct") &&
             result;
    result = check(isotp_send(&ctx, replacement, sizeof(replacement)) ==
                       -EBUSY &&
                       sent_frame_count == 1u && tx_done_count == 0u,
                   "repeated TX is rejected without a callback") && result;
    result = send_fc(&ctx, 0u, 0u, 0u) && result;
    result = check(sent_frame_count == 2u && tx_done_count == 1u &&
                       last_tx_status == 0 && !isotp_tx_busy(&ctx),
                   "single CF completes the eight-byte transfer") && result;
    result = check_tx_frame(&sent_frames[1], 0x21u, &eight[6], 2u) && result;
    isotp_deinit(&ctx);

    reset_capture();
    init_context(&ctx, &driver);
    result = check(isotp_send(&ctx, one, 0u) == -EINVAL,
                   "zero-length TX is rejected") && result;
    result = check(isotp_send(&ctx, too_long, ISOTP_MAX_LEN + 1u) == -EINVAL,
                   "4096-byte TX is rejected") && result;
    result = check(sent_frame_count == 0u && tx_done_count == 0u &&
                       !isotp_tx_busy(&ctx),
                   "invalid TX lengths do not send or callback") && result;
    isotp_deinit(&ctx);
    return result;
}

static bool scenario_tx_boundary(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    uint8_t payload[ISOTP_MAX_LEN];
    uint16_t offset = 6u;
    uint8_t sequence_number = 1u;
    bool result = true;

    reset_capture();
    for (size_t index = 0; index < sizeof(payload); ++index) {
        payload[index] = (uint8_t)((index * 29u + 3u) & 0xFFu);
    }
    init_context(&ctx, &driver);
    result = check(isotp_send(&ctx, payload, sizeof(payload)) == 0,
                   "4095-byte TX is accepted") && result;
    result = check(sent_frame_count == 1u && isotp_tx_busy(&ctx),
                   "4095-byte TX sends one FF and waits for FC") && result;
    result = check(sent_frames[0].data[0] == 0x1Fu &&
                       sent_frames[0].data[1] == 0xFFu &&
                       memcmp(&sent_frames[0].data[2], payload, 6u) == 0,
                   "4095-byte FF is encoded correctly") && result;
    result = send_fc(&ctx, 0u, 0u, 0u) && result;
    while (tx_done_count == 0u) {
        result = drive_stmin_timer(&ctx, sent_frame_count + 1u) && result;
    }
    result = check(sent_frame_count == 586u && tx_done_count == 1u &&
                       last_tx_status == 0 && !isotp_tx_busy(&ctx),
                   "4095-byte TX emits all CFs and completes once") && result;

    for (size_t index = 1u;
         index < sizeof(sent_frames) / sizeof(sent_frames[0]) &&
         index < sent_frame_count;
         ++index) {
        uint16_t remaining = (uint16_t)(ISOTP_MAX_LEN - offset);
        uint8_t payload_len = remaining > 7u ? 7u : (uint8_t)remaining;

        result = check(sent_frames[index].data[0] ==
                           (uint8_t)(0x20u | sequence_number),
                       "4095-byte CF sequence number is correct") && result;
        result = check(sent_frames[index].dlc == CAN_CLASSIC_DLEN,
                       "4095-byte CF uses DLC 8") && result;
        result = check(memcmp(&sent_frames[index].data[1], &payload[offset],
                              payload_len) == 0,
                       "4095-byte CF payload is correct") && result;
        result = check(check_padding(&sent_frames[index],
                                     (uint8_t)(1u + payload_len)),
                       "4095-byte CF padding is correct") && result;
        offset = (uint16_t)(offset + payload_len);
        sequence_number = (uint8_t)((sequence_number + 1u) & 0x0Fu);
    }
    result = check(offset == ISOTP_MAX_LEN,
                   "4095-byte TX consumes the complete payload") && result;
    isotp_deinit(&ctx);
    return result;
}

static bool scenario_tx_flow_control(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    can_frame_t invalid_fc;
    can_frame_t single_frame;
    uint8_t payload[36];
    bool result = true;

    for (size_t index = 0; index < sizeof(payload); ++index) {
        payload[index] = (uint8_t)(index + 1u);
    }
    reset_capture();
    init_context(&ctx, &driver);
    result = check(isotp_send(&ctx, payload, sizeof(payload)) == 0,
                   "flow-control payload is accepted") && result;
    result = send_fc(&ctx, 3u, 4u, 0x0Au) && result;
    result = check(sent_frame_count == 1u && isotp_tx_busy(&ctx),
                   "unknown FC status is ignored") && result;
    init_frame(&invalid_fc);
    invalid_fc.data[0] = 0x30u;
    invalid_fc.data[3] = 0x00u;
    result = check(isotp_on_frame(&ctx, &invalid_fc) == 0 &&
                       sent_frame_count == 1u && isotp_tx_busy(&ctx),
                   "invalid FC padding is ignored") && result;
    init_frame(&single_frame);
    single_frame.data[0] = 0x02u;
    single_frame.data[1] = 0x10u;
    single_frame.data[2] = 0x01u;
    result = check(isotp_on_frame(&ctx, &single_frame) == 0 &&
                       capture.count == 1u && isotp_tx_busy(&ctx),
                   "RX SF does not change TX WAIT_FC state") && result;
    result = send_fc(&ctx, 1u, 4u, 0x0Au) && result;
    result = check(sent_frame_count == 1u && isotp_tx_busy(&ctx),
                   "FC Wait does not send CF and keeps N_Bs active") && result;
    result = send_fc(&ctx, 0u, 4u, 0x0Au) && result;
    result = check(sent_frame_count == 2u && isotp_tx_busy(&ctx),
                   "FC CTS sends the first CF immediately") && result;
    for (unsigned int index = 0u; index < 3u; ++index) {
        result = drive_stmin_timer(&ctx, sent_frame_count + 1u) && result;
    }
    result = check(sent_frame_count == 5u && isotp_tx_busy(&ctx) &&
                       tx_done_count == 0u,
                   "BS=4 pauses after four CFs") && result;
    for (size_t index = 2u; index <= 4u; ++index) {
        result = check(elapsed_us(&sent_times[index - 1u],
                                  &sent_times[index]) >= 5000u,
                       "STmin=10ms meets the timing lower bound") && result;
    }
    result = send_fc(&ctx, 1u, 4u, 0x0Au) && result;
    result = check(sent_frame_count == 5u && isotp_tx_busy(&ctx),
                   "FC Wait after a block keeps TX paused") && result;
    result = send_fc(&ctx, 0u, 4u, 0u) && result;
    result = check(sent_frame_count == 6u && tx_done_count == 1u &&
                       last_tx_status == 0 && !isotp_tx_busy(&ctx),
                   "remaining CF completes after the next CTS") && result;
    result = check_tx_frame(&sent_frames[5], 0x25u, &payload[34], 2u) &&
             result;
    isotp_deinit(&ctx);
    return result;
}

static bool scenario_tx_stmin_codes(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    const uint8_t payload[20] = {
        1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u,
        11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u,
    };
    bool result = true;

    reset_capture();
    init_context(&ctx, &driver);
    result = check(isotp_send(&ctx, payload, sizeof(payload)) == 0,
                   "microsecond STmin payload is accepted") && result;
    result = send_fc(&ctx, 0u, 0u, 0xF1u) && result;
    result = check(sent_frame_count == 2u && tx_done_count == 0u,
                   "STmin 0xF1 delays the next CF") && result;
    {
        const struct timespec delay = {0, 1000000L};

        (void)nanosleep(&delay, NULL);
    }
    result = check(isotp_on_timer(&ctx, ISOTP_TIMER_STMIN) == 0 &&
                       sent_frame_count == 3u && tx_done_count == 1u,
                   "STmin 0xF1 eventually sends the next CF") && result;
    isotp_deinit(&ctx);

    reset_capture();
    init_context(&ctx, &driver);
    result = check(isotp_send(&ctx, payload, sizeof(payload)) == 0,
                   "reserved STmin payload is accepted") && result;
    result = send_fc(&ctx, 0u, 0u, 0x80u) && result;
    result = check(sent_frame_count == 2u && tx_done_count == 0u,
                   "reserved STmin delays the next CF") && result;
    {
        const struct timespec delay = {0, 10000000L};

        (void)nanosleep(&delay, NULL);
    }
    result = check(isotp_on_timer(&ctx, ISOTP_TIMER_STMIN) == 0 &&
                       sent_frame_count == 2u && tx_done_count == 0u,
                   "reserved STmin is not treated as zero") && result;
    while (tx_done_count == 0u) {
        result = drive_stmin_timer(&ctx, sent_frame_count + 1u) && result;
    }
    result = check(sent_frame_count == 3u && last_tx_status == 0,
                   "reserved STmin eventually sends the next CF") && result;
    isotp_deinit(&ctx);
    return result;
}

static bool scenario_tx_abort(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    const uint8_t payload[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    const isotp_stats_t *stats;
    bool result = true;

    reset_capture();
    init_context(&ctx, &driver);
    result = check(isotp_send(&ctx, payload, sizeof(payload)) == 0,
                   "Overflow test payload is accepted") && result;
    result = send_fc(&ctx, 2u, 0u, 0u) && result;
    result = check(sent_frame_count == 1u && tx_done_count == 1u &&
                       last_tx_status == -EMSGSIZE && !isotp_tx_busy(&ctx),
                   "FC Overflow aborts TX with the documented status") &&
             result;
    isotp_deinit(&ctx);

    reset_capture();
    init_context(&ctx, &driver);
    result = check(isotp_send(&ctx, payload, sizeof(payload)) == 0,
                   "N_Bs test payload is accepted") && result;
    result = force_timer_expiry(&ctx, ISOTP_TIMER_N_BS) && result;
    result = check(isotp_on_timer(&ctx, ISOTP_TIMER_N_BS) == 0,
                   "N_Bs timer is handled") && result;
    stats = isotp_get_stats(&ctx);
    result = check(sent_frame_count == 1u && tx_done_count == 1u &&
                       last_tx_status == -ETIMEDOUT && !isotp_tx_busy(&ctx),
                   "N_Bs timeout aborts TX once") && result;
    result = check(stats != NULL && stats->timeouts == 1u,
                   "N_Bs timeout is counted") && result;
    isotp_deinit(&ctx);
    return result;
}

static bool scenario_tx_can_error(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    can_frame_t flow_control;
    const uint8_t short_payload[] = {1u};
    const uint8_t multi_payload[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    bool result = true;

    reset_capture();
    init_context(&ctx, &driver);
    forced_send_status = -EIO;
    result = check(isotp_send(&ctx, short_payload, sizeof(short_payload)) == 0,
                   "SF send failure is reported asynchronously") && result;
    result = check(tx_done_count == 1u && last_tx_status == -EIO &&
                       !isotp_tx_busy(&ctx),
                   "SF CAN failure completes TX with the CAN error") && result;
    isotp_deinit(&ctx);

    reset_capture();
    init_context(&ctx, &driver);
    forced_send_status = -ENETDOWN;
    result = check(isotp_send(&ctx, multi_payload, sizeof(multi_payload)) == 0,
                   "FF send failure is reported asynchronously") && result;
    result = check(tx_done_count == 1u && last_tx_status == -ENETDOWN &&
                       !isotp_tx_busy(&ctx),
                   "FF CAN failure completes TX with the CAN error") && result;
    isotp_deinit(&ctx);

    reset_capture();
    init_context(&ctx, &driver);
    forced_send_status = 0;
    result = check(isotp_send(&ctx, multi_payload, sizeof(multi_payload)) == 0,
                   "CF failure payload is accepted") && result;
    forced_send_status = -EIO;
    init_frame(&flow_control);
    flow_control.data[0] = 0x30u;
    result = check(isotp_on_frame(&ctx, &flow_control) == -EIO,
                   "FC handling returns the CF CAN error") && result;
    result = check(tx_done_count == 1u && last_tx_status == -EIO &&
                       !isotp_tx_busy(&ctx),
                   "CF CAN failure completes TX with the CAN error") && result;
    isotp_deinit(&ctx);
    return result;
}

static bool scenario_tx_cleanup_error(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    const uint8_t payload[] = {1u};
    int stmin_fd;
    bool result = true;

    reset_capture();
    init_context(&ctx, &driver);
    stmin_fd = isotp_timer_fd(&ctx, ISOTP_TIMER_STMIN);
    /* Fault injection only; normal callers must not close borrowed fds. */
    result = check(stmin_fd >= 0 && close(stmin_fd) == 0,
                   "test can invalidate the borrowed STmin timer") && result;
    result = check(isotp_send(&ctx, payload, sizeof(payload)) == 0,
                   "cleanup failure still accepts the SF request") && result;
    result = check(tx_done_count == 1u && last_tx_status == -EBADF &&
                       !isotp_tx_busy(&ctx),
                   "timer cleanup failure reaches the completion callback") &&
             result;
    isotp_deinit(&ctx);
    return result;
}

static bool scenario_tx_wait_restart(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    const uint8_t payload[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    bool result = true;

    reset_capture();
    init_context(&ctx, &driver);
    result = check(isotp_send(&ctx, payload, sizeof(payload)) == 0,
                   "N_Bs restart payload is accepted") && result;
    result = check(sleep_milliseconds(700L),
                   "initial N_Bs wait completes") && result;
    result = send_fc(&ctx, 1u, 0u, 0u) && result;
    result = check(sleep_milliseconds(400L),
                   "post-Wait interval completes") && result;
    result = check(isotp_on_timer(&ctx, ISOTP_TIMER_N_BS) == 0 &&
                       tx_done_count == 0u && isotp_tx_busy(&ctx),
                   "FC Wait restarts N_Bs before the old deadline") && result;
    result = check(sleep_milliseconds(700L),
                   "restarted N_Bs wait completes") && result;
    result = check(isotp_on_timer(&ctx, ISOTP_TIMER_N_BS) == 0 &&
                       tx_done_count == 1u && last_tx_status == -ETIMEDOUT &&
                       !isotp_tx_busy(&ctx),
                   "restarted N_Bs eventually aborts the transfer") && result;
    isotp_deinit(&ctx);
    return result;
}

static bool init_uds_context(isotp_ctx_t *isotp, uds_ctx_t *uds,
                             can_driver_t *driver)
{
    isotp_config_t config = {0};
    int result;

    config.can = driver;
    config.rx_id = TEST_RX_ID;
    config.tx_id = TEST_TX_ID;
    config.on_message = uds_on_isotp_message;
    config.on_tx_done = uds_on_isotp_tx_done;
    config.user = uds;
    result = isotp_init(isotp, &config);
    if (result < 0) {
        return false;
    }
    result = uds_init(uds, isotp);
    if (result < 0) {
        isotp_deinit(isotp);
        return false;
    }
    return true;
}

static bool scenario_uds_busy(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t isotp = {0};
    uds_ctx_t uds = {0};
    const uint8_t tx_payload[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    const uint8_t first_request[2] = {0x10u, 0x01u};
    const uint8_t second_request[2] = {0x22u, 0x01u};
    bool result = true;

    reset_capture();
    result = check(init_uds_context(&isotp, &uds, &driver),
                   "UDS busy context initializes") && result;
    result = check(isotp_send(&isotp, tx_payload, sizeof(tx_payload)) == 0,
                   "busy test starts a real multi-frame TX") && result;
    result = send_sf(&isotp, first_request, sizeof(first_request)) && result;
    result = send_sf(&isotp, second_request, sizeof(second_request)) && result;
    result = send_fc(&isotp, 0u, 0u, 0u) && result;
    result = check(sent_frame_count == 3u && !isotp_tx_busy(&isotp),
                   "TX completion clears busy slot and sends one NRC") &&
             result;
    result = check(sent_frames[2].data[0] == 0x03u &&
                       sent_frames[2].data[1] == 0x7Fu &&
                       sent_frames[2].data[2] == first_request[0] &&
                       sent_frames[2].data[3] == UDS_NRC_BUSY_REPEAT_REQUEST,
                   "busy completion response is NRC 0x21") && result;
    uds_deinit(&uds);
    isotp_deinit(&isotp);

    reset_capture();
    result = check(init_uds_context(&isotp, &uds, &driver),
                   "UDS timeout context initializes") && result;
    result = check(isotp_send(&isotp, tx_payload, sizeof(tx_payload)) == 0,
                   "timeout busy test starts TX") && result;
    result = send_sf(&isotp, first_request, sizeof(first_request)) && result;
    result = force_timer_expiry(&isotp, ISOTP_TIMER_N_BS) && result;
    result = check(isotp_on_timer(&isotp, ISOTP_TIMER_N_BS) == 0,
                   "busy TX timeout is handled") && result;
    result = check(sent_frame_count == 2u &&
                       sent_frames[1].data[1] == 0x7Fu &&
                       sent_frames[1].data[2] == first_request[0] &&
                       sent_frames[1].data[3] == UDS_NRC_BUSY_REPEAT_REQUEST,
                   "busy timeout also sends one NRC 0x21") && result;
    uds_deinit(&uds);
    isotp_deinit(&isotp);
    return result;
}

static bool scenario_boundary(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    uint8_t payload[ISOTP_MAX_LEN];
    uint16_t offset = 0u;
    uint8_t sn = 1u;
    bool result = true;

    reset_capture();
    for (size_t index = 0; index < sizeof(payload); ++index) {
        payload[index] = (uint8_t)((index * 37u + 11u) & 0xFFu);
    }
    init_context(&ctx, &driver);
    result = send_ff(&ctx, ISOTP_MAX_LEN, payload) && check_fc();
    offset = 6u;
    while (offset < ISOTP_MAX_LEN) {
        uint16_t remaining = (uint16_t)(ISOTP_MAX_LEN - offset);
        uint8_t count = remaining > 7u ? 7u : (uint8_t)remaining;

        result = send_cf(&ctx, sn, &payload[offset], count) && result;
        offset = (uint16_t)(offset + count);
        sn = (uint8_t)((sn + 1u) & 0x0Fu);
    }
    result = check(capture.count == 1u, "one boundary callback") && result;
    result = check(capture.len == ISOTP_MAX_LEN, "4095-byte payload length") &&
             result;
    result = check(memcmp(capture.data, payload, sizeof(payload)) == 0,
                   "4095-byte payload contents") && result;
    isotp_deinit(&ctx);
    return result;
}

static bool scenario_sn_error(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    const uint8_t first[6] = {1u, 2u, 3u, 4u, 5u, 6u};
    const uint8_t next[7] = {7u, 8u, 9u, 10u, 11u, 12u, 13u};
    can_frame_t sf;
    const isotp_stats_t *stats;
    bool result = true;

    reset_capture();
    init_context(&ctx, &driver);
    result = send_ff(&ctx, 20u, first) && check_fc();
    result = send_cf(&ctx, 2u, next, sizeof(next)) && result;
    stats = isotp_get_stats(&ctx);
    result = check(capture.count == 0u, "SN error does not callback") && result;
    result = check(stats != NULL && stats->sn_errors == 1u,
                   "SN error is counted") && result;

    init_frame(&sf);
    sf.data[0] = 0x02u;
    sf.data[1] = 0x10u;
    sf.data[2] = 0x01u;
    result = check(isotp_on_frame(&ctx, &sf) == 0,
                   "new SF is accepted after SN error") && result;
    result = check(capture.count == 1u && capture.len == 2u &&
                       capture.data[0] == 0x10u && capture.data[1] == 0x01u,
                   "RX recovers after SN error") && result;
    isotp_deinit(&ctx);
    return result;
}

static bool force_timer_expiry(isotp_ctx_t *ctx, isotp_timer_t timer_type)
{
    unsigned int timeout_ms;

    if (isotp_timer_fd(ctx, timer_type) < 0) {
        return false;
    }
    timeout_ms = timer_type == ISOTP_TIMER_N_CR ? ISOTP_N_CR_MS :
                 ISOTP_N_BS_MS;
    return sleep_milliseconds((long)timeout_ms + 50L);
}

static bool drive_stmin_timer(isotp_ctx_t *ctx, size_t target_frame_count)
{
    const struct timespec delay = {0, 1000000L};

    for (unsigned int attempt = 0u; attempt < 2000u; ++attempt) {
        int result;

        if (sent_frame_count >= target_frame_count) {
            return true;
        }
        (void)nanosleep(&delay, NULL);
        result = isotp_on_timer(ctx, ISOTP_TIMER_STMIN);
        if (result < 0) {
            return false;
        }
    }
    return sent_frame_count >= target_frame_count;
}

static bool sleep_milliseconds(long milliseconds)
{
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000L;
    delay.tv_nsec = (milliseconds % 1000L) * 1000000L;
    return nanosleep(&delay, NULL) == 0;
}

static uint64_t elapsed_us(const struct timespec *start,
                           const struct timespec *end)
{
    int64_t seconds = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    int64_t nanoseconds = (int64_t)end->tv_nsec -
                          (int64_t)start->tv_nsec;

    return (uint64_t)(seconds * 1000000LL + nanoseconds / 1000LL);
}

static bool scenario_timeout(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    const uint8_t first[6] = {1u, 2u, 3u, 4u, 5u, 6u};
    can_frame_t sf;
    const isotp_stats_t *stats;
    bool result = true;

    reset_capture();
    init_context(&ctx, &driver);
    result = send_ff(&ctx, 20u, first) && check_fc() && result;
    result = force_timer_expiry(&ctx, ISOTP_TIMER_N_CR) && result;
    result = check(isotp_on_timer(&ctx, ISOTP_TIMER_N_CR) == 0,
                   "N_Cr timer is handled") && result;
    stats = isotp_get_stats(&ctx);
    result = check(capture.count == 0u, "timeout does not callback") && result;
    result = check(stats != NULL && stats->timeouts == 1u,
                   "N_Cr timeout is counted") && result;

    init_frame(&sf);
    sf.data[0] = 0x02u;
    sf.data[1] = 0x22u;
    sf.data[2] = 0x01u;
    result = check(isotp_on_frame(&ctx, &sf) == 0,
                   "new SF is accepted after timeout") && result;
    result = check(capture.count == 1u, "RX recovers after timeout") && result;
    isotp_deinit(&ctx);
    return result;
}

static bool scenario_preempt(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    const uint8_t first[6] = {1u, 2u, 3u, 4u, 5u, 6u};
    const uint8_t replacement[6] = {9u, 8u, 7u, 6u, 5u, 4u};
    const uint8_t final_bytes[2] = {3u, 2u};
    bool result = true;

    reset_capture();
    init_context(&ctx, &driver);
    result = send_ff(&ctx, 20u, first) && result;
    result = send_ff(&ctx, 8u, replacement) && result;
    result = send_cf(&ctx, 1u, final_bytes, sizeof(final_bytes)) && result;
    result = check(capture.count == 1u && capture.len == 8u,
                   "new FF replaces old partial message") && result;
    result = check(memcmp(capture.data, replacement, sizeof(replacement)) == 0 &&
                       capture.data[6] == 3u && capture.data[7] == 2u,
                   "replacement FF contents are delivered") && result;
    isotp_deinit(&ctx);

    reset_capture();
    init_context(&ctx, &driver);
    result = send_ff(&ctx, 20u, first) && result;
    {
        can_frame_t sf;

        init_frame(&sf);
        sf.data[0] = 0x02u;
        sf.data[1] = 0x10u;
        sf.data[2] = 0x01u;
        result = check(isotp_on_frame(&ctx, &sf) == 0,
                       "new SF replaces old partial message") && result;
    }
    result = check(capture.count == 1u && capture.len == 2u &&
                       capture.data[0] == 0x10u && capture.data[1] == 0x01u,
                   "replacement SF contents are delivered") && result;
    isotp_deinit(&ctx);
    return result;
}

static bool scenario_strict(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t ctx = {0};
    can_frame_t frame;
    const uint8_t first[6] = {1u, 2u, 3u, 4u, 5u, 6u};
    const uint8_t final_bytes[2] = {7u, 8u};
    bool result = true;

    reset_capture();
    init_context(&ctx, &driver);

    init_frame(&frame);
    frame.dlc = 7u;
    frame.data[0] = 0x10u;
    frame.data[1] = 0x08u;
    result = check(isotp_on_frame(&ctx, &frame) == 0,
                   "short FF is ignored") && result;
    result = check(sent_frame_count == 0u, "short FF sends no FC") && result;

    init_frame(&frame);
    frame.data[0] = 0x02u;
    frame.data[1] = 0x10u;
    frame.data[2] = 0x01u;
    frame.data[3] = 0x00u;
    result = check(isotp_on_frame(&ctx, &frame) == 0,
                   "bad SF padding is ignored") && result;
    result = check(capture.count == 0u, "bad SF padding does not callback") &&
             result;

    result = send_ff(&ctx, 8u, first) && result;
    frame.data[0] = 0x21u;
    frame.data[1] = final_bytes[0];
    frame.data[2] = final_bytes[1];
    frame.data[3] = 0x00u;
    result = check(isotp_on_frame(&ctx, &frame) == 0,
                   "bad CF padding is ignored") && result;
    result = check(capture.count == 0u, "bad CF padding does not callback") &&
             result;
    frame.data[3] = ISOTP_PADDING_BYTE;
    result = check(isotp_on_frame(&ctx, &frame) == 0,
                   "valid final CF is accepted") && result;
    result = check(capture.count == 1u && capture.len == 8u,
                   "strict frame recovery works") && result;
    isotp_deinit(&ctx);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fprintf(stderr,
                      "usage: %s basic|tx-basic|tx-boundary|tx-flow|"
                      "tx-stmin|tx-abort|tx-wait|uds-busy|boundary|sn-error|"
                      "tx-can-error|tx-cleanup-error|timeout|"
                      "preempt|strict\n",
                      argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "basic") == 0) {
        return scenario_basic() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "tx-basic") == 0) {
        return scenario_tx_basic() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "tx-boundary") == 0) {
        return scenario_tx_boundary() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "tx-flow") == 0) {
        return scenario_tx_flow_control() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "tx-stmin") == 0) {
        return scenario_tx_stmin_codes() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "tx-abort") == 0) {
        return scenario_tx_abort() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "tx-can-error") == 0) {
        return scenario_tx_can_error() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "tx-cleanup-error") == 0) {
        return scenario_tx_cleanup_error() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "tx-wait") == 0) {
        return scenario_tx_wait_restart() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "uds-busy") == 0) {
        return scenario_uds_busy() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "boundary") == 0) {
        return scenario_boundary() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "sn-error") == 0) {
        return scenario_sn_error() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "timeout") == 0) {
        return scenario_timeout() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "preempt") == 0) {
        return scenario_preempt() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "strict") == 0) {
        return scenario_strict() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    (void)fprintf(stderr, "unknown scenario: %s\n", argv[1]);
    return EXIT_FAILURE;
}
