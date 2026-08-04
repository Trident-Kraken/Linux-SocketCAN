#ifndef VUDS_ISOTP_H
#define VUDS_ISOTP_H

#include "can_driver.h"

#include <stdbool.h>
#include <stdint.h>

#define ISOTP_MAX_LEN 4095u
#define ISOTP_SF_MAX_LEN 7u
#define ISOTP_N_BS_MS 1000u
#define ISOTP_N_CR_MS 1000u
#define ISOTP_PADDING_BYTE 0xAAu

typedef void (*isotp_rx_cb_t)(const uint8_t *data, uint16_t len, void *user);
typedef void (*isotp_tx_done_cb_t)(int status, void *user);

typedef struct {
    can_frame_sender_t frame_sender;
    uint32_t           rx_id;
    uint32_t           tx_id;
    isotp_rx_cb_t      on_message;
    isotp_tx_done_cb_t on_tx_done;
    void              *user;
} isotp_config_t;

typedef enum {
    ISOTP_TX_IDLE,
    ISOTP_TX_WAIT_FC,
    ISOTP_TX_WAIT_STMIN,
} isotp_tx_state_t;

typedef enum {
    ISOTP_RX_IDLE,
    ISOTP_RX_WAIT_CF,
} isotp_rx_state_t;

typedef enum {
    ISOTP_TIMER_N_BS,
    ISOTP_TIMER_N_CR,
    ISOTP_TIMER_STMIN,
    ISOTP_TIMER_COUNT,
} isotp_timer_t;

typedef struct {
    uint64_t timeouts;
    uint64_t sn_errors;
} isotp_stats_t;

typedef struct {
    isotp_config_t   config;
    isotp_tx_state_t tx_state;
    isotp_rx_state_t rx_state;
    int              timer_fds[ISOTP_TIMER_COUNT];
    uint8_t          rx_buffer[ISOTP_MAX_LEN];
    uint8_t          tx_buffer[ISOTP_MAX_LEN];
    uint16_t         rx_len;
    uint16_t         rx_total_len;
    uint8_t          rx_expected_sn;
    uint16_t         tx_total_len;
    uint16_t         tx_offset;
    uint8_t          tx_next_sn;
    uint8_t          tx_block_size;
    uint8_t          tx_block_count;
    uint32_t         tx_stmin_us;
    bool             initialized;
    isotp_stats_t    stats;
} isotp_ctx_t;

int                  isotp_init(isotp_ctx_t *ctx,
                                const isotp_config_t *config);
void                 isotp_deinit(isotp_ctx_t *ctx);
int                  isotp_send(isotp_ctx_t *ctx, const uint8_t *data,
                                uint16_t len);
bool                 isotp_tx_busy(const isotp_ctx_t *ctx);
int                  isotp_on_frame(isotp_ctx_t *ctx,
                                    const can_frame_t *frame);
int                  isotp_timer_fd(const isotp_ctx_t *ctx,
                                    isotp_timer_t timer);
int                  isotp_on_timer(isotp_ctx_t *ctx,
                                    isotp_timer_t timer);
const isotp_stats_t *isotp_get_stats(const isotp_ctx_t *ctx);

#endif
