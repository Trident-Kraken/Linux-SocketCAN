#define _POSIX_C_SOURCE 200809L

#include "isotp.h"

#include <errno.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define ISOTP_PCI_TYPE_MASK 0xF0u
#define ISOTP_PCI_SF 0x00u
#define ISOTP_SF_LEN_MASK 0x0Fu

/*
 * T003 active transitions:
 *
 * RX_IDLE -- valid SF --> RX_IDLE -- on_message()
 * TX_IDLE -- send SF  --> TX_IDLE -- on_tx_done()
 *
 * WAIT_CF/WAIT_FC/WAIT_STMIN are reserved for T004/T005. Unknown PCI types
 * are ignored without changing either state machine.
 */

static bool isotp_timer_valid(isotp_timer_t timer)
{
    return timer >= ISOTP_TIMER_N_BS && timer < ISOTP_TIMER_COUNT;
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
    uint8_t sf_len;

    if (ctx == NULL || frame == NULL) {
        return -EINVAL;
    }
    if (!ctx->initialized) {
        return -ENODEV;
    }
    if (frame->id != ctx->config.rx_id || frame->dlc < 1u ||
        frame->dlc > CAN_CLASSIC_DLEN) {
        return 0;
    }
    if ((frame->data[0] & ISOTP_PCI_TYPE_MASK) != ISOTP_PCI_SF) {
        return 0;
    }

    sf_len = frame->data[0] & ISOTP_SF_LEN_MASK;
    if (sf_len == 0u || sf_len > ISOTP_SF_MAX_LEN ||
        sf_len > frame->dlc - 1u) {
        return 0;
    }

    (void)memcpy(ctx->rx_buffer, &frame->data[1], sf_len);
    ctx->rx_len = sf_len;
    ctx->rx_state = ISOTP_RX_IDLE;
    ctx->config.on_message(ctx->rx_buffer, ctx->rx_len,
                           ctx->config.user);
    ctx->rx_len = 0u;
    return 0;
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

    return 0;
}

const isotp_stats_t *isotp_get_stats(const isotp_ctx_t *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }
    return &ctx->stats;
}
