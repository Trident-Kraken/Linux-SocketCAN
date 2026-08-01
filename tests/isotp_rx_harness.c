#define _POSIX_C_SOURCE 200809L

#include "isotp.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>

#define TEST_RX_ID 0x7E0u
#define TEST_TX_ID 0x7E8u

typedef struct {
    uint8_t data[ISOTP_MAX_LEN];
    uint16_t len;
    unsigned int count;
} capture_t;

static can_frame_t sent_frames[4];
static size_t sent_frame_count;
static capture_t capture;

int can_send(can_driver_t *driver, const can_frame_t *frame)
{
    (void)driver;
    if (frame == NULL || sent_frame_count >= sizeof(sent_frames) /
                            sizeof(sent_frames[0])) {
        return -EINVAL;
    }
    sent_frames[sent_frame_count++] = *frame;
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
    (void)status;
    (void)user;
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
    sent_frame_count = 0u;
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

static bool force_timer_expiry(isotp_ctx_t *ctx)
{
    struct itimerspec timer = {0};
    int fd = isotp_timer_fd(ctx, ISOTP_TIMER_N_CR);

    timer.it_value.tv_nsec = 1000000L;
    if (fd < 0 || timerfd_settime(fd, 0, &timer, NULL) < 0) {
        (void)fprintf(stderr, "cannot arm test timer: %s\n", strerror(errno));
        return false;
    }
    timer.it_value.tv_nsec = 5000000L;
    (void)nanosleep(&timer.it_value, NULL);
    return true;
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
    result = force_timer_expiry(&ctx) && result;
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
                      "usage: %s basic|boundary|sn-error|timeout|preempt|strict\n",
                      argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "basic") == 0) {
        return scenario_basic() ? EXIT_SUCCESS : EXIT_FAILURE;
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
