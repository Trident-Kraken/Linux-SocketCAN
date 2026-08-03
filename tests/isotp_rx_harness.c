#define _POSIX_C_SOURCE 200809L

#include "isotp.h"
#include "uds.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/timerfd.h>
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
static bool uds_s3_timer_active(const uds_ctx_t *uds);
static bool set_uds_s3_timer_ms(const uds_ctx_t *uds,
                                unsigned int milliseconds);

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

static int count_open_fds(void)
{
    DIR *directory;
    struct dirent *entry;
    int count = 0;

    directory = opendir("/proc/self/fd");
    if (directory == NULL) {
        return -errno;
    }
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] != '.') {
            count++;
        }
    }
    (void)closedir(directory);
    return count - 1;
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

static bool scenario_uds_session_s3(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t isotp = {0};
    uds_ctx_t uds = {0};
    const uint8_t enter_extended[] = {0x10u, 0x03u};
    const uint8_t suppress_extended[] = {0x10u, 0x83u};
    const uint8_t tester_present[] = {UDS_SID_TESTER_PRESENT, 0x80u};
    const uint8_t tester_present_response[] = {
        UDS_SID_TESTER_PRESENT + UDS_POSITIVE_RESPONSE_OFFSET, 0x00u,
    };
    const uint8_t tx_payload[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    const uint8_t session_response[] = {
        UDS_SID_DIAGNOSTIC_SESSION_CONTROL + UDS_POSITIVE_RESPONSE_OFFSET,
        0x03u, 0x00u, 0x32u, 0x01u, 0xF4u,
    };
    struct itimerspec timer_after = {0};
    bool result = true;

    reset_capture();
    result = check(init_uds_context(&isotp, &uds, &driver),
                   "UDS session context initializes") && result;
    result = send_sf(&isotp, enter_extended, sizeof(enter_extended)) && result;
    result = check(sent_frame_count == 1u &&
                       check_tx_frame(&sent_frames[0], 0x06u,
                                      session_response,
                                      sizeof(session_response)),
                   "10 03 sends the extended-session response") && result;
    result = check(uds.session == UDS_SESSION_EXTENDED &&
                       uds.security_state == UDS_SECURITY_LOCKED &&
                       uds_s3_timer_active(&uds),
                   "10 03 enters extended session and arms S3") && result;

    result = sleep_milliseconds(20L) && result;
    reset_capture();
    result = send_sf(&isotp, tester_present, sizeof(tester_present)) && result;
    result = check(sent_frame_count == 0u && uds_s3_timer_active(&uds),
                   "3E 80 suppresses its response and keeps S3 active") &&
             result;
    result = check(timerfd_gettime(uds_s3_timer_fd(&uds), &timer_after) == 0 &&
                       timer_after.it_value.tv_sec >= 4,
                   "valid TesterPresent re-arms the five-second S3 timer") &&
             result;

    result = send_sf(&isotp, suppress_extended, sizeof(suppress_extended)) &&
             result;
    result = check(sent_frame_count == 0u &&
                       uds.session == UDS_SESSION_EXTENDED,
                   "10 83 changes session without a response") && result;

    uds.security_state = UDS_SECURITY_SEED_SENT;
    result = check(set_uds_s3_timer_ms(&uds, 1u),
                   "test shortens S3 for expiry verification") && result;
    result = sleep_milliseconds(5L) && result;
    result = check(uds_on_s3_timer(&uds) == 0 &&
                       uds.session == UDS_SESSION_DEFAULT &&
                       uds.security_state == UDS_SECURITY_LOCKED &&
                       !uds_s3_timer_active(&uds),
                   "S3 expiry returns to default and re-locks security") &&
             result;

    reset_capture();
    {
        const uint8_t ordinary_tester_present[] = {
            UDS_SID_TESTER_PRESENT, 0x00u,
        };

        result = send_sf(&isotp, ordinary_tester_present,
                         sizeof(ordinary_tester_present)) && result;
    }
    result = check(sent_frame_count == 1u &&
                       check_tx_frame(&sent_frames[0], 0x02u,
                                      tester_present_response,
                                      sizeof(tester_present_response)),
                   "3E 00 sends 7E 00 in default session") && result;

    reset_capture();
    {
        const uint8_t short_tester_present[] = {UDS_SID_TESTER_PRESENT};
        const uint8_t short_response[] = {
            UDS_NEGATIVE_RESPONSE_SID, UDS_SID_TESTER_PRESENT,
            UDS_NRC_INVALID_FORMAT,
        };

        result = send_sf(&isotp, short_tester_present,
                         sizeof(short_tester_present)) && result;
        result = check(sent_frame_count == 1u &&
                           check_tx_frame(&sent_frames[0], 0x03u,
                                          short_response,
                                          sizeof(short_response)),
                       "short TesterPresent returns NRC 0x13") && result;
    }

    reset_capture();
    {
        const uint8_t invalid_tester_present[] = {
            UDS_SID_TESTER_PRESENT, 0x01u,
        };
        const uint8_t invalid_response[] = {
            UDS_NEGATIVE_RESPONSE_SID, UDS_SID_TESTER_PRESENT,
            UDS_NRC_SUBFUNCTION_NOT_SUPPORTED,
        };

        result = send_sf(&isotp, invalid_tester_present,
                         sizeof(invalid_tester_present)) && result;
        result = check(sent_frame_count == 1u &&
                           check_tx_frame(&sent_frames[0], 0x03u,
                                          invalid_response,
                                          sizeof(invalid_response)),
                       "invalid TesterPresent returns NRC 0x12") && result;
    }

    reset_capture();
    result = send_sf(&isotp, enter_extended, sizeof(enter_extended)) && result;
    result = check(set_uds_s3_timer_ms(&uds, 50u),
                   "test shortens S3 for busy-gate verification") && result;
    result = check(isotp_send(&isotp, tx_payload, sizeof(tx_payload)) == 0,
                   "busy-gate test starts a multi-frame TX") && result;
    result = sleep_milliseconds(20L) && result;
    reset_capture();
    result = send_sf(&isotp, tester_present, sizeof(tester_present)) && result;
    result = check(sent_frame_count == 0u &&
                       timerfd_gettime(uds_s3_timer_fd(&uds), &timer_after) ==
                           0 &&
                       timer_after.it_value.tv_sec == 0 &&
                       timer_after.it_value.tv_nsec < 100000000L,
                   "TX busy request does not re-arm S3") && result;
    uds_deinit(&uds);
    uds_deinit(&uds);
    isotp_deinit(&isotp);
    isotp_deinit(&isotp);
    return result;
}

static bool scenario_uds_security(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t isotp = {0};
    uds_ctx_t uds = {0};
    const uint8_t enter_extended[] = {UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
                                      0x03u};
    const uint8_t request_seed[] = {UDS_SID_SECURITY_ACCESS, 0x01u};
    uint8_t key_request[6] = {UDS_SID_SECURITY_ACCESS, 0x02u,
                              0u, 0u, 0u, 0u};
    uint32_t seed = 0u;
    uint32_t wrong_key;
    uint32_t old_seed;
    uint64_t active_lock_until;
    uint32_t busy_seed;
    bool busy_seed_valid;
    uint8_t busy_failed_attempts;
    uint64_t busy_lock_until;
    const uint8_t expected_seed_prefix[] = {
        UDS_SID_SECURITY_ACCESS + UDS_POSITIVE_RESPONSE_OFFSET, 0x01u,
    };
    const uint8_t expected_unlock[] = {
        UDS_SID_SECURITY_ACCESS + UDS_POSITIVE_RESPONSE_OFFSET, 0x02u,
    };
    bool result = true;

    reset_capture();
    result = check(uds_compute_key(0x12345678u) == 0xF8FE924Cu,
                   "Seed-Key algorithm matches the specification") && result;
    result = check(init_uds_context(&isotp, &uds, &driver),
                   "UDS security context initializes") && result;

    result = send_sf(&isotp, request_seed, sizeof(request_seed)) && result;
    result = check(sent_frame_count == 1u &&
                       sent_frames[0].data[1] == UDS_NEGATIVE_RESPONSE_SID &&
                       sent_frames[0].data[2] == UDS_SID_SECURITY_ACCESS &&
                       sent_frames[0].data[3] ==
                           UDS_NRC_SERVICE_NOT_SUPPORTED_IN_SESSION,
                   "27 01 is restricted to the extended session") && result;

    reset_capture();
    result = send_sf(&isotp, enter_extended, sizeof(enter_extended)) && result;
    reset_capture();
    result = send_sf(&isotp, key_request, sizeof(key_request)) && result;
    result = check(sent_frame_count == 1u &&
                       sent_frames[0].data[3] == UDS_NRC_REQUEST_SEQUENCE_ERROR,
                   "sendKey without a seed returns NRC 0x24") && result;

    reset_capture();
    result = send_sf(&isotp, request_seed, sizeof(request_seed)) && result;
    result = check(sent_frame_count == 1u &&
                       sent_frames[0].data[0] == 0x06u &&
                       sent_frames[0].data[1] == expected_seed_prefix[0] &&
                       sent_frames[0].data[2] == expected_seed_prefix[1],
                   "requestSeed returns a non-degenerate seed") && result;
    seed = ((uint32_t)sent_frames[0].data[3] << 24u) |
           ((uint32_t)sent_frames[0].data[4] << 16u) |
           ((uint32_t)sent_frames[0].data[5] << 8u) |
           (uint32_t)sent_frames[0].data[6];
    result = check(seed != 0u && seed != UINT32_MAX,
                   "requestSeed is not all zero or all 0xFF") && result;
    result = check(uds.security_state == UDS_SECURITY_SEED_SENT &&
                       uds.security_seed_valid,
                   "requestSeed enters SEED_SENT") && result;

    wrong_key = uds_compute_key(seed) ^ 0x01u;
    key_request[2] = (uint8_t)(wrong_key >> 24u);
    key_request[3] = (uint8_t)(wrong_key >> 16u);
    key_request[4] = (uint8_t)(wrong_key >> 8u);
    key_request[5] = (uint8_t)wrong_key;
    reset_capture();
    result = send_sf(&isotp, key_request, sizeof(key_request)) && result;
    result = check(sent_frames[0].data[3] == UDS_NRC_INVALID_KEY &&
                       uds.security_failed_attempts == 1u,
                   "first wrong key returns NRC 0x35") && result;
    reset_capture();
    result = send_sf(&isotp, key_request, sizeof(key_request)) && result;
    result = check(sent_frames[0].data[3] == UDS_NRC_INVALID_KEY &&
                       uds.security_failed_attempts == 2u,
                   "second wrong key returns NRC 0x35") && result;
    reset_capture();
    result = send_sf(&isotp, key_request, sizeof(key_request)) && result;
    result = check(sent_frames[0].data[3] == UDS_NRC_ATTEMPTS_EXCEEDED &&
                       uds.security_state == UDS_SECURITY_LOCKED &&
                       !uds.security_seed_valid &&
                       uds.security_lock_until_ns != 0u,
                   "third wrong key starts the lockout") && result;

    active_lock_until = uds.security_lock_until_ns;
    reset_capture();
    result = send_sf(&isotp, (const uint8_t[]){
                         UDS_SID_DIAGNOSTIC_SESSION_CONTROL, 0x01u},
                     2u) && result;
    reset_capture();
    result = send_sf(&isotp, enter_extended, sizeof(enter_extended)) && result;
    reset_capture();
    result = send_sf(&isotp, request_seed, sizeof(request_seed)) && result;
    result = check(sent_frames[0].data[3] == UDS_NRC_DELAY_NOT_EXPIRED &&
                       uds.security_lock_until_ns == active_lock_until,
                   "active lockout survives a session switch") && result;

    reset_capture();
    result = send_sf(&isotp, request_seed, sizeof(request_seed)) && result;
    result = check(sent_frames[0].data[3] == UDS_NRC_DELAY_NOT_EXPIRED,
                   "requestSeed is delayed during lockout") && result;
    uds.security_lock_until_ns = 1u;
    reset_capture();
    result = send_sf(&isotp, request_seed, sizeof(request_seed)) && result;
    result = check(sent_frame_count == 1u &&
                       sent_frames[0].data[1] == expected_seed_prefix[0] &&
                       uds.security_failed_attempts == 0u,
                   "expired lockout recovers on requestSeed") && result;
    seed = ((uint32_t)sent_frames[0].data[3] << 24u) |
           ((uint32_t)sent_frames[0].data[4] << 16u) |
           ((uint32_t)sent_frames[0].data[5] << 8u) |
           (uint32_t)sent_frames[0].data[6];
    old_seed = seed;
    key_request[2] = (uint8_t)(uds_compute_key(seed) >> 24u);
    key_request[3] = (uint8_t)(uds_compute_key(seed) >> 16u);
    key_request[4] = (uint8_t)(uds_compute_key(seed) >> 8u);
    key_request[5] = (uint8_t)uds_compute_key(seed);
    reset_capture();
    result = send_sf(&isotp, key_request, sizeof(key_request)) && result;
    result = check(check_tx_frame(&sent_frames[0], 0x02u, expected_unlock,
                                  sizeof(expected_unlock)) &&
                       uds.security_state == UDS_SECURITY_UNLOCKED &&
                       !uds.security_seed_valid,
                   "correct key unlocks and invalidates the seed") && result;

    reset_capture();
    result = send_sf(&isotp, request_seed, sizeof(request_seed)) && result;
    seed = ((uint32_t)sent_frames[0].data[3] << 24u) |
           ((uint32_t)sent_frames[0].data[4] << 16u) |
           ((uint32_t)sent_frames[0].data[5] << 8u) |
           (uint32_t)sent_frames[0].data[6];
    result = send_sf(&isotp, (const uint8_t[]){
                         UDS_SID_DIAGNOSTIC_SESSION_CONTROL, 0x01u},
                     2u) && result;
    result = send_sf(&isotp, enter_extended, sizeof(enter_extended)) && result;
    key_request[2] = (uint8_t)(uds_compute_key(seed) >> 24u);
    key_request[3] = (uint8_t)(uds_compute_key(seed) >> 16u);
    key_request[4] = (uint8_t)(uds_compute_key(seed) >> 8u);
    key_request[5] = (uint8_t)uds_compute_key(seed);
    reset_capture();
    result = send_sf(&isotp, key_request, sizeof(key_request)) && result;
    result = check(sent_frames[0].data[3] == UDS_NRC_REQUEST_SEQUENCE_ERROR,
                   "session switch invalidates the old seed") && result;

    reset_capture();
    result = send_sf(&isotp, request_seed, sizeof(request_seed)) && result;
    old_seed = ((uint32_t)sent_frames[0].data[3] << 24u) |
               ((uint32_t)sent_frames[0].data[4] << 16u) |
               ((uint32_t)sent_frames[0].data[5] << 8u) |
               (uint32_t)sent_frames[0].data[6];
    result = check(set_uds_s3_timer_ms(&uds, 1u),
                   "shortens S3 for security seed expiry") && result;
    result = sleep_milliseconds(5L) && result;
    result = check(uds_on_s3_timer(&uds) == 0 &&
                       uds.session == UDS_SESSION_DEFAULT,
                   "S3 expiry resets the security session") && result;
    result = send_sf(&isotp, enter_extended, sizeof(enter_extended)) && result;
    key_request[2] = (uint8_t)(uds_compute_key(old_seed) >> 24u);
    key_request[3] = (uint8_t)(uds_compute_key(old_seed) >> 16u);
    key_request[4] = (uint8_t)(uds_compute_key(old_seed) >> 8u);
    key_request[5] = (uint8_t)uds_compute_key(old_seed);
    reset_capture();
    result = send_sf(&isotp, key_request, sizeof(key_request)) && result;
    result = check(sent_frames[0].data[3] == UDS_NRC_REQUEST_SEQUENCE_ERROR,
                   "S3 expiry invalidates the old seed") && result;

    reset_capture();
    uds.security_seed = 0x10203040u;
    uds.security_seed_valid = true;
    uds.security_state = UDS_SECURITY_SEED_SENT;
    uds.security_failed_attempts = 2u;
    uds.security_lock_until_ns = UINT64_MAX;
    busy_seed = uds.security_seed;
    busy_seed_valid = uds.security_seed_valid;
    busy_failed_attempts = uds.security_failed_attempts;
    busy_lock_until = uds.security_lock_until_ns;
    result = check(isotp_send(&isotp, (const uint8_t[]){1u, 2u, 3u, 4u,
                                                        5u, 6u, 7u, 8u},
                              8u) == 0,
                   "security busy test starts TX") && result;
    result = send_sf(&isotp, request_seed, sizeof(request_seed)) && result;
    result = check(sent_frame_count == 1u &&
                       uds.security_state == UDS_SECURITY_SEED_SENT &&
                       uds.security_seed == busy_seed &&
                       uds.security_seed_valid == busy_seed_valid &&
                       uds.security_failed_attempts == busy_failed_attempts &&
                       uds.security_lock_until_ns == busy_lock_until,
                   "busy 27 request does not change security state") && result;
    result = send_fc(&isotp, 0u, 0u, 0u) && result;
    result = check(sent_frames[2].data[1] == UDS_NEGATIVE_RESPONSE_SID &&
                       sent_frames[2].data[2] == UDS_SID_SECURITY_ACCESS &&
                       sent_frames[2].data[3] == UDS_NRC_BUSY_REPEAT_REQUEST,
                   "busy 27 request receives NRC 0x21") && result;

    uds_deinit(&uds);
    isotp_deinit(&isotp);
    return result;
}

static bool scenario_uds_did(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t isotp = {0};
    uds_ctx_t uds = {0};
    const uint8_t enter_extended[] = {UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
                                      0x03u};
    const uint8_t enter_default[] = {UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
                                     0x01u};
    const uint8_t read_temperature[] = {UDS_SID_READ_DATA_BY_IDENTIFIER,
                                        0x01u, 0x00u};
    const uint8_t read_vin[] = {UDS_SID_READ_DATA_BY_IDENTIFIER,
                                0xF1u, 0x90u};
    const uint8_t request_seed[] = {UDS_SID_SECURITY_ACCESS, 0x01u};
    const uint8_t write_period[] = {UDS_SID_WRITE_DATA_BY_IDENTIFIER,
                                    0x02u, 0x00u, 0x01u, 0x2Cu};
    const uint8_t read_period[] = {UDS_SID_READ_DATA_BY_IDENTIFIER,
                                   0x02u, 0x00u};
    uint8_t key_request[6] = {UDS_SID_SECURITY_ACCESS, 0x02u,
                              0u, 0u, 0u, 0u};
    uint8_t write_vin[20] = {UDS_SID_WRITE_DATA_BY_IDENTIFIER,
                             0xF1u, 0x90u};
    const uint8_t vin[] = "LVUDS2026TEST0002";
    uint32_t seed;
    bool result = true;

    (void)memcpy(&write_vin[3], vin, sizeof(vin) - 1u);
    reset_capture();
    result = check(init_uds_context(&isotp, &uds, &driver),
                   "UDS DID context initializes") && result;

    result = send_sf(&isotp, read_temperature, sizeof(read_temperature)) &&
             result;
    result = check(sent_frame_count == 1u &&
                       check_tx_frame(&sent_frames[0], 0x05u,
                                      (const uint8_t[]){0x62u, 0x01u, 0x00u,
                                                       0x00u, 0x55u},
                                      5u),
                   "short DID returns its static value") && result;

    reset_capture();
    result = send_sf(&isotp, read_vin, sizeof(read_vin)) && result;
    result = check(sent_frame_count == 1u && sent_frames[0].data[0] == 0x10u &&
                       sent_frames[0].data[1] == 0x14u &&
                       sent_frames[0].data[2] == 0x62u &&
                       sent_frames[0].data[3] == 0xF1u &&
                       sent_frames[0].data[4] == 0x90u &&
                       sent_frames[0].data[5] == 0x4Cu &&
                       sent_frames[0].data[6] == 0x56u &&
                       sent_frames[0].data[7] == 0x55u,
                   "VIN read starts a 20-byte multi-frame response") && result;
    result = send_fc(&isotp, 0u, 0u, 0u) && result;
    result = drive_stmin_timer(&isotp, 3u) && result;
    result = check(sent_frame_count == 3u &&
                       check_tx_frame(&sent_frames[1], 0x21u,
                                      (const uint8_t[]){0x44u, 0x53u, 0x32u,
                                                       0x30u, 0x32u, 0x36u,
                                                       0x44u},
                                      7u) &&
                       check_tx_frame(&sent_frames[2], 0x22u,
                                      (const uint8_t[]){0x45u, 0x4Du, 0x4Fu,
                                                       0x30u, 0x30u, 0x30u,
                                                       0x31u},
                                      7u),
                   "VIN read sends both consecutive frames") && result;

    reset_capture();
    result = send_sf(&isotp, enter_extended, sizeof(enter_extended)) && result;
    reset_capture();
    result = send_sf(&isotp, request_seed, sizeof(request_seed)) && result;
    seed = ((uint32_t)sent_frames[0].data[3] << 24u) |
           ((uint32_t)sent_frames[0].data[4] << 16u) |
           ((uint32_t)sent_frames[0].data[5] << 8u) |
           (uint32_t)sent_frames[0].data[6];
    key_request[2] = (uint8_t)(uds_compute_key(seed) >> 24u);
    key_request[3] = (uint8_t)(uds_compute_key(seed) >> 16u);
    key_request[4] = (uint8_t)(uds_compute_key(seed) >> 8u);
    key_request[5] = (uint8_t)uds_compute_key(seed);
    reset_capture();
    result = send_sf(&isotp, key_request, sizeof(key_request)) && result;
    result = check(check_tx_frame(&sent_frames[0], 0x02u,
                                  (const uint8_t[]){0x67u, 0x02u}, 2u),
                   "DID test unlocks through SecurityAccess") && result;

    reset_capture();
    result = send_sf(&isotp, write_period, sizeof(write_period)) && result;
    result = check(check_tx_frame(&sent_frames[0], 0x03u,
                                  (const uint8_t[]){0x6Eu, 0x02u, 0x00u}, 3u),
                   "writable DID returns a positive response") && result;
    reset_capture();
    result = send_sf(&isotp, read_period, sizeof(read_period)) && result;
    result = check(check_tx_frame(&sent_frames[0], 0x05u,
                                  (const uint8_t[]){0x62u, 0x02u, 0x00u,
                                                   0x01u, 0x2Cu},
                                  5u),
                   "writable DID reads back the new value") && result;

    reset_capture();
    result = send_sf(&isotp, read_vin, sizeof(read_vin)) && result;
    result = send_sf(&isotp, write_period, sizeof(write_period)) && result;
    result = check(sent_frame_count == 1u && sent_frames[0].data[0] == 0x10u,
                   "busy write is held while VIN response is active") && result;
    result = send_fc(&isotp, 0u, 0u, 0u) && result;
    result = drive_stmin_timer(&isotp, 4u) && result;
    result = check(sent_frame_count == 4u &&
                       sent_frames[3].data[1] == UDS_NEGATIVE_RESPONSE_SID &&
                       sent_frames[3].data[2] ==
                           UDS_SID_WRITE_DATA_BY_IDENTIFIER &&
                       sent_frames[3].data[3] == UDS_NRC_BUSY_REPEAT_REQUEST,
                   "busy write receives NRC 0x21 after VIN TX") && result;
    reset_capture();
    result = send_sf(&isotp, read_period, sizeof(read_period)) && result;
    result = check(check_tx_frame(&sent_frames[0], 0x05u,
                                  (const uint8_t[]){0x62u, 0x02u, 0x00u,
                                                   0x01u, 0x2Cu},
                                  5u),
                   "busy write does not change the DID") && result;

    reset_capture();
    result = send_ff(&isotp, sizeof(write_vin), write_vin) && check_fc() &&
             result;
    result = send_cf(&isotp, 1u, &write_vin[6], 7u) && result;
    result = send_cf(&isotp, 2u, &write_vin[13], 7u) && result;
    result = check(sent_frame_count == 2u &&
                       check_tx_frame(&sent_frames[1], 0x03u,
                                      (const uint8_t[]){0x6Eu, 0xF1u, 0x90u},
                                      3u),
                   "long VIN write uses the RX multi-frame path") && result;

    reset_capture();
    result = send_sf(&isotp, enter_default, sizeof(enter_default)) && result;
    reset_capture();
    result = send_sf(&isotp, enter_extended, sizeof(enter_extended)) && result;
    reset_capture();
    result = send_sf(&isotp, write_period, sizeof(write_period)) && result;
    result = check(sent_frames[0].data[3] == UDS_NRC_SECURITY_ACCESS_DENIED,
                   "session switch re-locks DID writes") && result;

    uds_deinit(&uds);
    isotp_deinit(&isotp);
    return result;
}

static bool scenario_uds_dtc(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t isotp = {0};
    uds_ctx_t uds = {0};
    const uint8_t all_dtcs[] = {UDS_SID_READ_DTC_INFORMATION,
                                UDS_DTC_REPORT_BY_STATUS_MASK, 0xFFu};
    const uint8_t confirmed_dtcs[] = {UDS_SID_READ_DTC_INFORMATION,
                                      UDS_DTC_REPORT_BY_STATUS_MASK, 0x08u};
    const uint8_t empty_dtcs[] = {UDS_SID_READ_DTC_INFORMATION,
                                  UDS_DTC_REPORT_BY_STATUS_MASK, 0x02u};
    const uint8_t invalid_subfunction[] = {UDS_SID_READ_DTC_INFORMATION,
                                           0x05u, 0x00u};
    const uint8_t short_request[] = {UDS_SID_READ_DTC_INFORMATION, 0x02u};
    const uint8_t extended_session[] = {UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
                                        0x03u};
    bool result = true;

    reset_capture();
    result = check(init_uds_context(&isotp, &uds, &driver),
                   "UDS DTC context initializes") && result;

    result = send_sf(&isotp, all_dtcs, sizeof(all_dtcs)) && result;
    result = check(sent_frame_count == 1u && sent_frames[0].data[0] == 0x10u &&
                       sent_frames[0].data[1] == 0x0Fu &&
                       sent_frames[0].data[2] == 0x59u &&
                       sent_frames[0].data[3] == 0x02u &&
                       sent_frames[0].data[4] == 0x09u &&
                       sent_frames[0].data[5] == 0x01u &&
                       sent_frames[0].data[6] == 0x02u &&
                       sent_frames[0].data[7] == 0x03u,
                   "19 02 FF starts the complete DTC response") && result;
    result = send_fc(&isotp, 0u, 0u, 0u) && result;
    result = drive_stmin_timer(&isotp, 3u) && result;
    result = check(sent_frame_count == 3u &&
                       check_tx_frame(&sent_frames[1], 0x21u,
                                      (const uint8_t[]){0x09u, 0x02u, 0x03u,
                                                       0x04u, 0x08u, 0x03u,
                                                       0x04u},
                                      7u) &&
                       check_tx_frame(&sent_frames[2], 0x22u,
                                      (const uint8_t[]){0x01u, 0x01u}, 2u),
                   "19 02 FF sends all three DTC records") && result;

    reset_capture();
    result = send_sf(&isotp, confirmed_dtcs, sizeof(confirmed_dtcs)) && result;
    result = check(sent_frames[0].data[0] == 0x10u &&
                       sent_frames[0].data[1] == 0x0Bu,
                   "19 02 08 reports two DTC records") && result;
    result = send_fc(&isotp, 0u, 0u, 0u) && result;
    result = check(sent_frame_count == 2u &&
                       check_tx_frame(&sent_frames[1], 0x21u,
                                      (const uint8_t[]){0x09u, 0x02u, 0x03u,
                                                       0x04u, 0x08u},
                                      5u),
                   "status mask filters out the current-only DTC") && result;

    reset_capture();
    result = send_sf(&isotp, empty_dtcs, sizeof(empty_dtcs)) && result;
    result = check(check_tx_frame(&sent_frames[0], 0x03u,
                                  (const uint8_t[]){0x59u, 0x02u, 0x09u}, 3u),
                   "no matching status returns a positive empty response") &&
             result;
    reset_capture();
    result = send_sf(&isotp, invalid_subfunction,
                     sizeof(invalid_subfunction)) && result;
    result = check(check_tx_frame(&sent_frames[0], 0x03u,
                                  (const uint8_t[]){UDS_NEGATIVE_RESPONSE_SID,
                                                   UDS_SID_READ_DTC_INFORMATION,
                                                   UDS_NRC_SUBFUNCTION_NOT_SUPPORTED},
                                  3u),
                   "unsupported DTC subfunction returns NRC 0x12") && result;
    reset_capture();
    result = send_sf(&isotp, short_request, sizeof(short_request)) && result;
    result = check(check_tx_frame(&sent_frames[0], 0x03u,
                                  (const uint8_t[]){UDS_NEGATIVE_RESPONSE_SID,
                                                   UDS_SID_READ_DTC_INFORMATION,
                                                   UDS_NRC_INVALID_FORMAT},
                                  3u),
                   "short DTC request returns NRC 0x13") && result;

    reset_capture();
    result = send_sf(&isotp, extended_session, sizeof(extended_session)) &&
             result;
    reset_capture();
    result = send_sf(&isotp, empty_dtcs, sizeof(empty_dtcs)) && result;
    result = check(check_tx_frame(&sent_frames[0], 0x03u,
                                  (const uint8_t[]){0x59u, 0x02u, 0x09u}, 3u),
                   "DTC service remains available in extended session") &&
             result;

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

static bool uds_s3_timer_active(const uds_ctx_t *uds)
{
    struct itimerspec value = {0};
    int timer_fd;

    timer_fd = uds_s3_timer_fd(uds);
    return timer_fd >= 0 && timerfd_gettime(timer_fd, &value) == 0 &&
           (value.it_value.tv_sec != 0 || value.it_value.tv_nsec != 0);
}

static bool set_uds_s3_timer_ms(const uds_ctx_t *uds,
                                unsigned int milliseconds)
{
    struct itimerspec value = {0};
    int timer_fd;

    timer_fd = uds_s3_timer_fd(uds);
    if (timer_fd < 0) {
        return false;
    }
    value.it_value.tv_sec = milliseconds / 1000u;
    value.it_value.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    return timerfd_settime(timer_fd, 0, &value, NULL) == 0;
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

static bool scenario_lifecycle(void)
{
    can_driver_t driver = {0};
    isotp_ctx_t isotp = {0};
    uds_ctx_t uds = {0};
    isotp_config_t config = {
        .can = &driver,
        .rx_id = TEST_RX_ID,
        .tx_id = TEST_TX_ID,
        .on_message = on_message,
        .on_tx_done = on_tx_done,
        .user = &capture,
    };
    struct rlimit original_limit;
    struct rlimit limited_limit;
    int open_fds;
    int probe_fd = -1;
    int bs_timer_fd;
    int s3_timer_fd;
    bool result = true;

    reset_capture();
    open_fds = count_open_fds();
    result = check(open_fds >= 0, "current fd count is available") && result;
    result = check(getrlimit(RLIMIT_NOFILE, &original_limit) == 0,
                   "original fd limit is available") && result;
    if (!result || (rlim_t)(open_fds + 1) > original_limit.rlim_max) {
        return false;
    }

    limited_limit = original_limit;
    limited_limit.rlim_cur = (rlim_t)(open_fds + 1);
    result = check(setrlimit(RLIMIT_NOFILE, &limited_limit) == 0,
                   "fd limit is reduced for ISO-TP init fault injection") && result;
    result = check(isotp_init(&isotp, &config) == -EMFILE,
                   "ISO-TP init returns EMFILE after partial timer creation") &&
             result;
    probe_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    result = check(probe_fd >= 0,
                   "failed ISO-TP init releases its partially created timer") &&
             result;
    if (probe_fd >= 0) {
        (void)close(probe_fd);
    }
    isotp_deinit(&isotp);
    isotp_deinit(&isotp);
    result = check(setrlimit(RLIMIT_NOFILE, &original_limit) == 0,
                   "original fd limit is restored") && result;
    if (!result) {
        return false;
    }

    result = check(isotp_init(&isotp, &config) == 0,
                   "ISO-TP reinitializes after failed init") && result;
    open_fds = count_open_fds();
    result = check(open_fds >= 0, "fd count is available before UDS init") &&
             result;
    if (!result) {
        isotp_deinit(&isotp);
        return false;
    }

    limited_limit = original_limit;
    limited_limit.rlim_cur = (rlim_t)open_fds;
    result = check(setrlimit(RLIMIT_NOFILE, &limited_limit) == 0,
                   "fd limit is reduced for UDS init fault injection") && result;
    result = check(uds_init(&uds, &isotp) == -EMFILE,
                   "UDS init returns EMFILE without retaining state") && result;
    uds_deinit(&uds);
    result = check(setrlimit(RLIMIT_NOFILE, &original_limit) == 0,
                   "fd limit is restored after UDS init failure") && result;
    result = check(uds_init(&uds, &isotp) == 0,
                   "UDS reinitializes after failed init") && result;
    s3_timer_fd = uds_s3_timer_fd(&uds);
    result = check(s3_timer_fd >= 0, "UDS exposes its active S3 timer") &&
             result;
    uds_deinit(&uds);
    result = check(uds_s3_timer_fd(&uds) == -1,
                   "UDS hides timer after deinit") && result;
    result = check(fcntl(s3_timer_fd, F_GETFD) == -1 && errno == EBADF,
                   "UDS deinit closes its S3 timer once") && result;
    uds_deinit(&uds);

    bs_timer_fd = isotp_timer_fd(&isotp, ISOTP_TIMER_N_BS);
    result = check(bs_timer_fd >= 0, "ISO-TP exposes its active N_Bs timer") &&
             result;
    isotp_deinit(&isotp);
    result = check(isotp_timer_fd(&isotp, ISOTP_TIMER_N_BS) == -1,
                   "ISO-TP hides timer after deinit") && result;
    result = check(fcntl(bs_timer_fd, F_GETFD) == -1 && errno == EBADF,
                   "ISO-TP deinit closes its timer once") && result;
    isotp_deinit(&isotp);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fprintf(stderr,
                      "usage: %s basic|tx-basic|tx-boundary|tx-flow|"
                      "tx-stmin|tx-abort|tx-wait|uds-busy|uds-security|uds-did|"
                      "uds-dtc|"
                      "boundary|sn-error|"
                      "tx-can-error|tx-cleanup-error|timeout|"
                      "preempt|strict|lifecycle\n",
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
    if (strcmp(argv[1], "uds-session-s3") == 0) {
        return scenario_uds_session_s3() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "uds-security") == 0) {
        return scenario_uds_security() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "uds-did") == 0) {
        return scenario_uds_did() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strcmp(argv[1], "uds-dtc") == 0) {
        return scenario_uds_dtc() ? EXIT_SUCCESS : EXIT_FAILURE;
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
    if (strcmp(argv[1], "lifecycle") == 0) {
        return scenario_lifecycle() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    (void)fprintf(stderr, "unknown scenario: %s\n", argv[1]);
    return EXIT_FAILURE;
}
