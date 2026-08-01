#ifndef VUDS_CAN_DRIVER_H
#define VUDS_CAN_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#define CAN_CLASSIC_DLEN 8u
#define CAN_STANDARD_ID_MAX 0x7FFu

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[CAN_CLASSIC_DLEN];
} can_frame_t;

typedef struct {
    uint64_t frames_rx;
    uint64_t frames_tx;
} can_stats_t;

typedef struct {
    int          fd;
    uint32_t     rx_id;
    bool         active;
    bool         verbose;
    can_stats_t  stats;
} can_driver_t;

int                can_open(can_driver_t *driver, const char *ifname,
                            uint32_t rx_id, bool verbose);
void               can_close(can_driver_t *driver);
int                can_get_fd(const can_driver_t *driver);
int                can_recv(can_driver_t *driver, can_frame_t *frame);
int                can_send(can_driver_t *driver, const can_frame_t *frame);
const can_stats_t *can_get_stats(const can_driver_t *driver);

#endif
