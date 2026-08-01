#define _POSIX_C_SOURCE 200809L

#include "isotp.h"

#include <errno.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define ISOTP_PCI_TYPE_MASK 0xF0u
#define ISOTP_PCI_SF 0x00u
#define ISOTP_PCI_FF 0x10u
#define ISOTP_PCI_CF 0x20u
#define ISOTP_PCI_FC 0x30u
#define ISOTP_SF_LEN_MASK 0x0Fu
#define ISOTP_FF_LEN_HIGH_MASK 0x0Fu
#define ISOTP_CF_SN_MASK 0x0Fu
#define ISOTP_FC_CTS 0x00u
#define ISOTP_FC_BS 0x00u
#define ISOTP_FC_STMIN 0x00u

/*
 * RX transitions:
 *
 * RX_IDLE -- valid SF --> RX_IDLE -- on_message()
 * RX_IDLE -- valid FF --> RX_WAIT_CF -- complete CF --> RX_IDLE
 * RX_WAIT_CF -- valid SF/FF --> replace old transfer
 * RX_WAIT_CF -- valid CF/SN error --> RX_IDLE
 * RX_WAIT_CF -- N_Cr timeout --> RX_IDLE
 *
 * TX transitions active in T003:
 * TX_IDLE -- send SF  --> TX_IDLE -- on_tx_done()
 *
 * TX_WAIT_FC/TX_WAIT_STMIN are reserved for T005. FC frames are ignored
 * until the TX state machine is implemented. Invalid frames never change
 * either state machine.
 */

static bool isotp_timer_valid(isotp_timer_t timer)
{
    return timer >= ISOTP_TIMER_N_BS && timer < ISOTP_TIMER_COUNT;
}

static int isotp_drain_timer(isotp_ctx_t *ctx, isotp_timer_t timer)
{
    uint64_t expirations;
    ssize_t bytes_read;

    do {
        bytes_read = read(ctx->timer_fds[timer], &expirations,
                          sizeof(expirations));
    } while (bytes_read < 0 && errno == EINTR);

    if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if (bytes_read < 0) {
        return -errno;
    }
    if ((size_t)bytes_read != sizeof(expirations)) {
        return -EIO;
    }
    return 0;
}

static int isotp_disarm_timer(isotp_ctx_t *ctx, isotp_timer_t timer)
{
    const struct itimerspec disarmed = {0};

    if (timerfd_settime(ctx->timer_fds[timer], 0, &disarmed, NULL) < 0) {
        return -errno;
    }
    return isotp_drain_timer(ctx, timer);
}

static int isotp_arm_timer(isotp_ctx_t *ctx, isotp_timer_t timer,
                           uint32_t milliseconds)
{
    struct itimerspec value = {0};
    int result;

    result = isotp_disarm_timer(ctx, timer);
    if (result < 0) {
        return result;
    }
    value.it_value.tv_sec = (time_t)(milliseconds / 1000u);
    value.it_value.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    if (timerfd_settime(ctx->timer_fds[timer], 0, &value, NULL) < 0) {
        return -errno;
    }
    return 0;
}

static int isotp_stop_timer(isotp_ctx_t *ctx, isotp_timer_t timer)
{
    return isotp_disarm_timer(ctx, timer);
}

static void isotp_reset_rx(isotp_ctx_t *ctx)
{
    (void)isotp_stop_timer(ctx, ISOTP_TIMER_N_CR);
    ctx->rx_state = ISOTP_RX_IDLE;
    ctx->rx_len = 0u;
    ctx->rx_total_len = 0u;
    ctx->rx_expected_sn = 0u;
}

static bool isotp_padding_valid(const can_frame_t *frame, uint8_t first_unused)
{
    for (uint8_t index = first_unused; index < CAN_CLASSIC_DLEN; ++index) {
        if (frame->data[index] != ISOTP_PADDING_BYTE) {
            return false;
        }
    }
    return true;
}

static int isotp_send_flow_control(isotp_ctx_t *ctx)
{
    can_frame_t frame;

    frame.id = ctx->config.tx_id;
    frame.dlc = CAN_CLASSIC_DLEN;
    (void)memset(frame.data, ISOTP_PADDING_BYTE, sizeof(frame.data));
    frame.data[0] = (uint8_t)(ISOTP_PCI_FC | ISOTP_FC_CTS);
    frame.data[1] = ISOTP_FC_BS;
    frame.data[2] = ISOTP_FC_STMIN;
    return can_send(ctx->config.can, &frame);
}

static int isotp_deliver_rx_message(isotp_ctx_t *ctx)
{
    uint16_t message_len = ctx->rx_len;

    isotp_reset_rx(ctx);
    ctx->config.on_message(ctx->rx_buffer, message_len, ctx->config.user);
    return 0;
}

static int isotp_handle_single_frame(isotp_ctx_t *ctx,
                                     const can_frame_t *frame)
{
    uint8_t sf_len = frame->data[0] & ISOTP_SF_LEN_MASK;

    if (sf_len == 0u || sf_len > ISOTP_SF_MAX_LEN ||
        !isotp_padding_valid(frame, (uint8_t)(1u + sf_len))) {
        return 0;
    }

    isotp_reset_rx(ctx);
    (void)memcpy(ctx->rx_buffer, &frame->data[1], sf_len);
    ctx->rx_len = sf_len;
    return isotp_deliver_rx_message(ctx);
}

static int isotp_handle_first_frame(isotp_ctx_t *ctx,
                                    const can_frame_t *frame)
{
    uint16_t total_len = (uint16_t)(((frame->data[0] &
                                      ISOTP_FF_LEN_HIGH_MASK) << 8) |
                                    frame->data[1]);
    int result;

    if (total_len < 8u || total_len > ISOTP_MAX_LEN) {
        return 0;
    }

    isotp_reset_rx(ctx);
    (void)memcpy(ctx->rx_buffer, &frame->data[2], 6u);
    ctx->rx_len = 6u;
    ctx->rx_total_len = total_len;
    ctx->rx_expected_sn = 1u;
    ctx->rx_state = ISOTP_RX_WAIT_CF;

    result = isotp_send_flow_control(ctx);
    if (result < 0) {
        isotp_reset_rx(ctx);
        return result;
    }
    result = isotp_arm_timer(ctx, ISOTP_TIMER_N_CR, ISOTP_N_CR_MS);
    if (result < 0) {
        isotp_reset_rx(ctx);
        return result;
    }
    return 0;
}

static int isotp_handle_consecutive_frame(isotp_ctx_t *ctx,
                                          const can_frame_t *frame)
{
    uint8_t sequence_number = frame->data[0] & ISOTP_CF_SN_MASK;
    uint16_t remaining;
    uint8_t payload_len;
    int result;

    if (ctx->rx_state != ISOTP_RX_WAIT_CF) {
        return 0;
    }
    remaining = (uint16_t)(ctx->rx_total_len - ctx->rx_len);
    payload_len = remaining > 7u ? 7u : (uint8_t)remaining;
    if (!isotp_padding_valid(frame, (uint8_t)(1u + payload_len))) {
        return 0;
    }
    if (sequence_number != ctx->rx_expected_sn) {
        ctx->stats.sn_errors++;
        isotp_reset_rx(ctx);
        return 0;
    }

    (void)memcpy(&ctx->rx_buffer[ctx->rx_len], &frame->data[1], payload_len);
    ctx->rx_len = (uint16_t)(ctx->rx_len + payload_len);
    if (ctx->rx_len == ctx->rx_total_len) {
        return isotp_deliver_rx_message(ctx);
    }

    ctx->rx_expected_sn = (uint8_t)((sequence_number + 1u) &
                                    ISOTP_CF_SN_MASK);
    result = isotp_arm_timer(ctx, ISOTP_TIMER_N_CR, ISOTP_N_CR_MS);
    if (result < 0) {
        isotp_reset_rx(ctx);
        return result;
    }
    return 0;
}

int isotp_init(isotp_ctx_t *ctx, const isotp_config_t *config)
{
    int timer_index;

    if (ctx == NULL || config == NULL || config->can == NULL ||
        config->on_message == NULL || config->on_tx_done == NULL ||
        config->rx_id > CAN_STANDARD_ID_MAX ||
        config->tx_id > CAN_STANDARD_ID_MAX) {
        return -EINVAL;
    }
    if (ctx->initialized) {
        return -EBUSY;
    }

    (void)memset(ctx, 0, sizeof(*ctx));
    for (timer_index = 0; timer_index < ISOTP_TIMER_COUNT; ++timer_index) {
        ctx->timer_fds[timer_index] = -1;
    }

    for (timer_index = 0; timer_index < ISOTP_TIMER_COUNT; ++timer_index) {
        int fd = timerfd_create(CLOCK_MONOTONIC,
                                TFD_NONBLOCK | TFD_CLOEXEC);

        if (fd < 0) {
            int result = -errno;

            while (timer_index > 0) {
                --timer_index;
                (void)close(ctx->timer_fds[timer_index]);
                ctx->timer_fds[timer_index] = -1;
            }
            return result;
        }
        ctx->timer_fds[timer_index] = fd;
    }

    ctx->config = *config;
    ctx->tx_state = ISOTP_TX_IDLE;
    ctx->rx_state = ISOTP_RX_IDLE;
    ctx->initialized = true;
    return 0;
}

void isotp_deinit(isotp_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->initialized) {
        return;
    }

    for (int timer_index = 0;
         timer_index < ISOTP_TIMER_COUNT; ++timer_index) {
        if (ctx->timer_fds[timer_index] >= 0) {
            (void)close(ctx->timer_fds[timer_index]);
        }
    }
    (void)memset(ctx, 0, sizeof(*ctx));
}

int isotp_send(isotp_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
    can_frame_t frame;
    int send_result;

    if (ctx == NULL || data == NULL || len == 0u || len > ISOTP_MAX_LEN) {
        return -EINVAL;
    }
    if (!ctx->initialized) {
        return -ENODEV;
    }
    if (ctx->tx_state != ISOTP_TX_IDLE) {
        return -EBUSY;
    }
    if (len > ISOTP_SF_MAX_LEN) {
        return -EMSGSIZE;
    }

    (void)memcpy(ctx->tx_buffer, data, len);
    ctx->tx_len = len;

    frame.id = ctx->config.tx_id;
    frame.dlc = CAN_CLASSIC_DLEN;
    (void)memset(frame.data, ISOTP_PADDING_BYTE, sizeof(frame.data));
    frame.data[0] = (uint8_t)len;
    (void)memcpy(&frame.data[1], ctx->tx_buffer, len);

    send_result = can_send(ctx->config.can, &frame);
    ctx->tx_len = 0u;
    ctx->tx_state = ISOTP_TX_IDLE;
    ctx->config.on_tx_done(send_result, ctx->config.user);
    return 0;
}

bool isotp_tx_busy(const isotp_ctx_t *ctx)
{
    return ctx != NULL && ctx->initialized &&
           ctx->tx_state != ISOTP_TX_IDLE;
}

int isotp_on_frame(isotp_ctx_t *ctx, const can_frame_t *frame)
{
    uint8_t pci_type;

    if (ctx == NULL || frame == NULL) {
        return -EINVAL;
    }
    if (!ctx->initialized) {
        return -ENODEV;
    }
    if (frame->id != ctx->config.rx_id || frame->dlc != CAN_CLASSIC_DLEN) {
        return 0;
    }

    pci_type = frame->data[0] & ISOTP_PCI_TYPE_MASK;
    switch (pci_type) {
    case ISOTP_PCI_SF:
        return isotp_handle_single_frame(ctx, frame);
    case ISOTP_PCI_FF:
        return isotp_handle_first_frame(ctx, frame);
    case ISOTP_PCI_CF:
        return isotp_handle_consecutive_frame(ctx, frame);
    case ISOTP_PCI_FC:
    default:
        return 0;
    }
}

int isotp_timer_fd(const isotp_ctx_t *ctx, isotp_timer_t timer)
{
    if (ctx == NULL || !ctx->initialized || !isotp_timer_valid(timer)) {
        return -1;
    }
    return ctx->timer_fds[timer];
}

int isotp_on_timer(isotp_ctx_t *ctx, isotp_timer_t timer)
{
    uint64_t expirations;
    ssize_t bytes_read;

    if (ctx == NULL || !ctx->initialized || !isotp_timer_valid(timer)) {
        return -EINVAL;
    }

    do {
        bytes_read = read(ctx->timer_fds[timer], &expirations,
                          sizeof(expirations));
    } while (bytes_read < 0 && errno == EINTR);

    if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if (bytes_read < 0) {
        return -errno;
    }
    if ((size_t)bytes_read != sizeof(expirations)) {
        return -EIO;
    }

    if (timer == ISOTP_TIMER_N_CR && ctx->rx_state == ISOTP_RX_WAIT_CF) {
        ctx->stats.timeouts++;
        isotp_reset_rx(ctx);
    }

    return 0;
}

const isotp_stats_t *isotp_get_stats(const isotp_ctx_t *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }
    return &ctx->stats;
}
