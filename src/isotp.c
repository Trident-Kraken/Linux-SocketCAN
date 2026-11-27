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
#define ISOTP_FC_WAIT 0x01u
#define ISOTP_FC_OVERFLOW 0x02u
#define ISOTP_FC_FS_MASK 0x0Fu
#define ISOTP_FC_BS 0x00u
#define ISOTP_FC_STMIN 0x00u
#define ISOTP_TIMER_MIN_NS 1L

/*
 * RX transitions:
 *
 * RX_IDLE -- valid SF --> RX_IDLE -- on_message()
 * RX_IDLE -- valid FF --> RX_WAIT_CF -- complete CF --> RX_IDLE
 * RX_WAIT_CF -- valid SF/FF --> replace old transfer
 * RX_WAIT_CF -- valid CF/SN error --> RX_IDLE
 * RX_WAIT_CF -- N_Cr timeout --> RX_IDLE
 *
 * TX transitions:
 * TX_IDLE -- send SF --> TX_IDLE -- on_tx_done()
 * TX_IDLE -- send FF --> TX_WAIT_FC -- FC CTS --> TX_WAIT_STMIN
 * TX_WAIT_FC -- FC Wait --> TX_WAIT_FC
 * TX_WAIT_STMIN -- timer --> TX_WAIT_STMIN or TX_WAIT_FC or TX_IDLE
 * TX_WAIT_FC -- timeout/Overflow --> TX_IDLE
 *
 * RX and TX transitions are independent. FC frames only affect TX, while
 * SF/FF/CF frames only affect RX.
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

static int isotp_arm_timer_us(isotp_ctx_t *ctx, isotp_timer_t timer,
                              uint32_t microseconds)
{
    struct itimerspec value = {0};
    int result;

    result = isotp_disarm_timer(ctx, timer);
    if (result < 0) {
        return result;
    }
    value.it_value.tv_sec = (time_t)(microseconds / 1000000u);
    value.it_value.tv_nsec = (long)(microseconds % 1000000u) * 1000L;
    if (microseconds == 0u) {
        /* A zero ISO-TP STmin still needs an epoll turn to avoid a loop. */
        value.it_value.tv_nsec = ISOTP_TIMER_MIN_NS;
    }
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

static int isotp_reset_tx(isotp_ctx_t *ctx)
{
    int first_error = 0;
    int result;

    result = isotp_stop_timer(ctx, ISOTP_TIMER_N_BS);
    if (result < 0) {
        first_error = result;
    }
    result = isotp_stop_timer(ctx, ISOTP_TIMER_STMIN);
    if (result < 0 && first_error == 0) {
        first_error = result;
    }
    ctx->tx_state = ISOTP_TX_IDLE;
    ctx->tx_total_len = 0u;
    ctx->tx_offset = 0u;
    ctx->tx_next_sn = 0u;
    ctx->tx_block_size = 0u;
    ctx->tx_block_count = 0u;
    ctx->tx_stmin_us = 0u;
    return first_error;
}

static void isotp_finish_tx(isotp_ctx_t *ctx, int status)
{
    int reset_result = isotp_reset_tx(ctx);

    if (reset_result < 0) {
        status = reset_result;
    }
    ctx->config.on_tx_done(status, ctx->config.user);
}

static int isotp_send_can_frame(isotp_ctx_t *ctx, const can_frame_t *frame)
{
    return ctx->config.frame_sender.send(ctx->config.frame_sender.context,
                                         frame);
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
    return isotp_send_can_frame(ctx, &frame);
}

static int isotp_send_first_frame(isotp_ctx_t *ctx)
{
    can_frame_t frame;

    frame.id = ctx->config.tx_id;
    frame.dlc = CAN_CLASSIC_DLEN;
    (void)memset(frame.data, ISOTP_PADDING_BYTE, sizeof(frame.data));
    frame.data[0] = (uint8_t)(ISOTP_PCI_FF |
                              ((ctx->tx_total_len >> 8) &
                               ISOTP_FF_LEN_HIGH_MASK));
    frame.data[1] = (uint8_t)(ctx->tx_total_len & 0xFFu);
    (void)memcpy(&frame.data[2], ctx->tx_buffer, 6u);
    return isotp_send_can_frame(ctx, &frame);
}

static int isotp_send_consecutive_frame(isotp_ctx_t *ctx,
                                        uint8_t *payload_len)
{
    can_frame_t frame;
    uint16_t remaining;
    uint8_t length;
    int result;

    if (ctx->tx_offset >= ctx->tx_total_len) {
        return -EINVAL;
    }

    remaining = (uint16_t)(ctx->tx_total_len - ctx->tx_offset);
    length = remaining > 7u ? 7u : (uint8_t)remaining;
    frame.id = ctx->config.tx_id;
    frame.dlc = CAN_CLASSIC_DLEN;
    (void)memset(frame.data, ISOTP_PADDING_BYTE, sizeof(frame.data));
    frame.data[0] = (uint8_t)(ISOTP_PCI_CF | ctx->tx_next_sn);
    (void)memcpy(&frame.data[1], &ctx->tx_buffer[ctx->tx_offset], length);
    result = isotp_send_can_frame(ctx, &frame);
    if (result < 0) {
        return result;
    }
    *payload_len = length;
    return 0;
}

static uint32_t isotp_decode_stmin(uint8_t stmin)
{
    if (stmin <= 0x7Fu) {
        return (uint32_t)stmin * 1000u;
    }
    if (stmin >= 0xF1u && stmin <= 0xF9u) {
        return (uint32_t)(stmin - 0xF0u) * 100u;
    }
    return 127000u;
}

static int isotp_send_next_consecutive_frame(isotp_ctx_t *ctx)
{
    uint8_t payload_len;
    int result;

    result = isotp_send_consecutive_frame(ctx, &payload_len);
    if (result < 0) {
        isotp_finish_tx(ctx, result);
        return result;
    }

    ctx->tx_offset = (uint16_t)(ctx->tx_offset + payload_len);
    ctx->tx_next_sn = (uint8_t)((ctx->tx_next_sn + 1u) &
                                ISOTP_CF_SN_MASK);
    if (ctx->tx_offset == ctx->tx_total_len) {
        isotp_finish_tx(ctx, 0);
        return 0;
    }

    if (ctx->tx_block_size != 0u) {
        ctx->tx_block_count++;
        if (ctx->tx_block_count == ctx->tx_block_size) {
            ctx->tx_block_count = 0u;
            ctx->tx_state = ISOTP_TX_WAIT_FC;
            result = isotp_arm_timer_us(ctx, ISOTP_TIMER_N_BS,
                                        ISOTP_N_BS_MS * 1000u);
            if (result < 0) {
                isotp_finish_tx(ctx, result);
                return result;
            }
            return 0;
        }
    }

    ctx->tx_state = ISOTP_TX_WAIT_STMIN;
    result = isotp_arm_timer_us(ctx, ISOTP_TIMER_STMIN,
                                ctx->tx_stmin_us);
    if (result < 0) {
        isotp_finish_tx(ctx, result);
        return result;
    }
    return 0;
}

static int isotp_handle_flow_control(isotp_ctx_t *ctx,
                                     const can_frame_t *frame)
{
    uint8_t flow_status = frame->data[0] & ISOTP_FC_FS_MASK;
    int result;

    if (ctx->tx_state != ISOTP_TX_WAIT_FC ||
        !isotp_padding_valid(frame, 3u) || flow_status > ISOTP_FC_OVERFLOW) {
        return 0;
    }

    if (flow_status == ISOTP_FC_WAIT) {
        result = isotp_arm_timer_us(ctx, ISOTP_TIMER_N_BS,
                                    ISOTP_N_BS_MS * 1000u);
        if (result < 0) {
            isotp_finish_tx(ctx, result);
        }
        return result;
    }
    if (flow_status == ISOTP_FC_OVERFLOW) {
        isotp_finish_tx(ctx, -EMSGSIZE);
        return 0;
    }

    result = isotp_stop_timer(ctx, ISOTP_TIMER_N_BS);
    if (result < 0) {
        isotp_finish_tx(ctx, result);
        return result;
    }
    ctx->tx_block_size = frame->data[1];
    ctx->tx_block_count = 0u;
    ctx->tx_stmin_us = isotp_decode_stmin(frame->data[2]);
    return isotp_send_next_consecutive_frame(ctx);
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
    bool was_waiting_cf;
    int result;

    if (total_len < 8u || total_len > ISOTP_MAX_LEN) {
        return 0;
    }

    was_waiting_cf = (ctx->rx_state == ISOTP_RX_WAIT_CF);
    isotp_reset_rx(ctx);
    (void)memcpy(ctx->rx_buffer, &frame->data[2], 6u);
    ctx->rx_len = 6u;
    ctx->rx_total_len = total_len;
    ctx->rx_expected_sn = 1u;
    ctx->rx_state = ISOTP_RX_WAIT_CF;

    result = isotp_send_flow_control(ctx);
    if (result < 0) {
        isotp_reset_rx(ctx);
        if (was_waiting_cf && ctx->config.on_rx_abort != NULL) {
            ctx->config.on_rx_abort(ctx->config.user);
        }
        return result;
    }
    result = isotp_arm_timer_us(ctx, ISOTP_TIMER_N_CR,
                                ISOTP_N_CR_MS * 1000u);
    if (result < 0) {
        isotp_reset_rx(ctx);
        if (was_waiting_cf && ctx->config.on_rx_abort != NULL) {
            ctx->config.on_rx_abort(ctx->config.user);
        }
        return result;
    }
    if (ctx->config.on_first_frame != NULL) {
        ctx->config.on_first_frame(ctx->config.user);
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
        if (ctx->config.on_rx_abort != NULL) {
            ctx->config.on_rx_abort(ctx->config.user);
        }
        return 0;
    }

    (void)memcpy(&ctx->rx_buffer[ctx->rx_len], &frame->data[1], payload_len);
    ctx->rx_len = (uint16_t)(ctx->rx_len + payload_len);
    if (ctx->rx_len == ctx->rx_total_len) {
        return isotp_deliver_rx_message(ctx);
    }

    ctx->rx_expected_sn = (uint8_t)((sequence_number + 1u) &
                                    ISOTP_CF_SN_MASK);
    result = isotp_arm_timer_us(ctx, ISOTP_TIMER_N_CR,
                                ISOTP_N_CR_MS * 1000u);
    if (result < 0) {
        isotp_reset_rx(ctx);
        if (ctx->config.on_rx_abort != NULL) {
            ctx->config.on_rx_abort(ctx->config.user);
        }
        return result;
    }
    return 0;
}

int isotp_init(isotp_ctx_t *ctx, const isotp_config_t *config)
{
    int timer_index;

    if (ctx == NULL || config == NULL || config->frame_sender.context == NULL ||
        config->frame_sender.send == NULL ||
        config->on_message == NULL || config->on_tx_done == NULL ||
        config->rx_id > CAN_STANDARD_ID_MAX ||
        config->tx_id > CAN_STANDARD_ID_MAX ||
        config->rx_id == config->tx_id) {
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
    (void)memcpy(ctx->tx_buffer, data, len);
    if (len <= ISOTP_SF_MAX_LEN) {
        frame.id = ctx->config.tx_id;
        frame.dlc = CAN_CLASSIC_DLEN;
        (void)memset(frame.data, ISOTP_PADDING_BYTE, sizeof(frame.data));
        frame.data[0] = (uint8_t)len;
        (void)memcpy(&frame.data[1], ctx->tx_buffer, len);

        send_result = isotp_send_can_frame(ctx, &frame);
        isotp_finish_tx(ctx, send_result);
        return 0;
    }

    ctx->tx_total_len = len;
    ctx->tx_offset = 6u;
    ctx->tx_next_sn = 1u;
    ctx->tx_block_size = 0u;
    ctx->tx_block_count = 0u;
    ctx->tx_stmin_us = 0u;
    ctx->tx_state = ISOTP_TX_WAIT_FC;

    send_result = isotp_send_first_frame(ctx);
    if (send_result < 0) {
        isotp_finish_tx(ctx, send_result);
        return 0;
    }
    send_result = isotp_arm_timer_us(ctx, ISOTP_TIMER_N_BS,
                                     ISOTP_N_BS_MS * 1000u);
    if (send_result < 0) {
        isotp_finish_tx(ctx, send_result);
    }
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
        return isotp_handle_flow_control(ctx, frame);
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
        if (ctx->config.on_rx_abort != NULL) {
            ctx->config.on_rx_abort(ctx->config.user);
        }
    } else if (timer == ISOTP_TIMER_N_BS &&
               ctx->tx_state == ISOTP_TX_WAIT_FC) {
        ctx->stats.timeouts++;
        isotp_finish_tx(ctx, -ETIMEDOUT);
    } else if (timer == ISOTP_TIMER_STMIN &&
               ctx->tx_state == ISOTP_TX_WAIT_STMIN) {
        return isotp_send_next_consecutive_frame(ctx);
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
