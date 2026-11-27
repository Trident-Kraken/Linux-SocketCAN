#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "can_driver.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void can_log_frame(const char *direction, const can_frame_t *frame)
{
    struct timespec now;
    struct tm local_time;
    char timestamp[32];

    if (clock_gettime(CLOCK_REALTIME, &now) != 0 ||
        localtime_r(&now.tv_sec, &local_time) == NULL) {
        (void)snprintf(timestamp, sizeof(timestamp), "--:--:--.---");
    } else {
        (void)snprintf(timestamp, sizeof(timestamp),
                       "%02d:%02d:%02d.%03ld",
                       local_time.tm_hour,
                       local_time.tm_min,
                       local_time.tm_sec,
                       now.tv_nsec / 1000000L);
    }

    (void)fprintf(stderr, "[%s] [DEBUG] [can] %s id=0x%03X dlc=%u data=",
                  timestamp, direction, frame->id, frame->dlc);
    for (uint8_t index = 0; index < frame->dlc; ++index) {
        (void)fprintf(stderr, "%02X%s", frame->data[index],
                      index + 1u == frame->dlc ? "" : " ");
    }
    (void)fputc('\n', stderr);
}

static int can_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -errno;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -errno;
    }
    return 0;
}

int can_open(can_driver_t *driver, const char *ifname, uint32_t rx_id,
             bool verbose)
{
    struct can_filter filter;
    struct ifreq interface_request;
    struct sockaddr_can address;
    int fd;
    int ifindex;
    int result;

    if (driver == NULL || ifname == NULL || ifname[0] == '\0' ||
        rx_id > CAN_STANDARD_ID_MAX) {
        return -EINVAL;
    }
    if (driver->active) {
        return -EBUSY;
    }
    if (strlen(ifname) >= IFNAMSIZ) {
        return -ENAMETOOLONG;
    }

    fd = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    if (fd < 0) {
        return -errno;
    }

    result = can_set_nonblocking(fd);
    if (result < 0) {
        (void)close(fd);
        return result;
    }

    (void)memset(&interface_request, 0, sizeof(interface_request));
    (void)strncpy(interface_request.ifr_name, ifname,
                  sizeof(interface_request.ifr_name) - 1u);
    if (ioctl(fd, SIOCGIFINDEX, &interface_request) < 0) {
        result = -errno;
        (void)close(fd);
        return result;
    }
    ifindex = interface_request.ifr_ifindex;

    if (ioctl(fd, SIOCGIFFLAGS, &interface_request) < 0) {
        result = -errno;
        (void)close(fd);
        return result;
    }
    if (((unsigned int)interface_request.ifr_flags & (unsigned int)IFF_UP) == 0u) {
        (void)close(fd);
        return -ENETDOWN;
    }

    /* Match only 11-bit standard data frames; reject EFF and RTR in kernel. */
    filter.can_id = rx_id;
    filter.can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &filter,
                   sizeof(filter)) < 0) {
        result = -errno;
        (void)close(fd);
        return result;
    }

    (void)memset(&address, 0, sizeof(address));
    address.can_family = AF_CAN;
    address.can_ifindex = ifindex;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        result = -errno;
        (void)close(fd);
        return result;
    }

    driver->fd = fd;
    driver->rx_id = rx_id;
    driver->verbose = verbose;
    driver->active = true;
    (void)memset(&driver->stats, 0, sizeof(driver->stats));
    return 0;
}

void can_close(can_driver_t *driver)
{
    if (driver == NULL) {
        return;
    }
    if (driver->active) {
        (void)close(driver->fd);
    }
    driver->fd = -1;
    driver->rx_id = 0;
    driver->active = false;
    driver->verbose = false;
}

int can_get_fd(const can_driver_t *driver)
{
    if (driver == NULL || !driver->active) {
        return -1;
    }
    return driver->fd;
}

int can_recv(can_driver_t *driver, can_frame_t *frame)
{
    struct can_frame kernel_frame;
    ssize_t received;

    if (driver == NULL || frame == NULL) {
        return -EINVAL;
    }
    if (!driver->active) {
        return -ENODEV;
    }

    for (;;) {
        received = recv(driver->fd, &kernel_frame, sizeof(kernel_frame), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return -EAGAIN;
            }
            return -errno;
        }
        if ((size_t)received != sizeof(kernel_frame)) {
            continue;
        }
        if ((kernel_frame.can_id &
             (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0u) {
            continue;
        }
        if (kernel_frame.can_dlc > CAN_CLASSIC_DLEN) {
            continue;
        }

        frame->id = kernel_frame.can_id & CAN_SFF_MASK;
        frame->dlc = kernel_frame.can_dlc;
        (void)memcpy(frame->data, kernel_frame.data, CAN_CLASSIC_DLEN);
        driver->stats.frames_rx++;
        if (driver->verbose) {
            can_log_frame("RX", frame);
        }
        return 0;
    }
}

int can_send(can_driver_t *driver, const can_frame_t *frame)
{
    struct can_frame kernel_frame;
    ssize_t sent;

    if (driver == NULL || frame == NULL || frame->id > CAN_STANDARD_ID_MAX ||
        frame->dlc > CAN_CLASSIC_DLEN) {
        return -EINVAL;
    }
    if (!driver->active) {
        return -ENODEV;
    }

    (void)memset(&kernel_frame, 0, sizeof(kernel_frame));
    kernel_frame.can_id = frame->id;
    kernel_frame.can_dlc = frame->dlc;
    if (frame->dlc > 0u) {
        (void)memcpy(kernel_frame.data, frame->data, frame->dlc);
    }

    do {
        sent = send(driver->fd, &kernel_frame, sizeof(kernel_frame),
                    MSG_DONTWAIT);
    } while (sent < 0 && errno == EINTR);

    if (sent < 0) {
        return -errno;
    }
    if ((size_t)sent != sizeof(kernel_frame)) {
        return -EIO;
    }

    driver->stats.frames_tx++;
    if (driver->verbose) {
        can_log_frame("TX", frame);
    }
    return 0;
}

static int can_driver_send_frame(void *context, const can_frame_t *frame)
{
    return can_send(context, frame);
}

can_frame_sender_t can_driver_frame_sender(can_driver_t *driver)
{
    return (can_frame_sender_t){
        .context = driver,
        .send = can_driver_send_frame,
    };
}

const can_stats_t *can_get_stats(const can_driver_t *driver)
{
    if (driver == NULL) {
        return NULL;
    }
    return &driver->stats;
}
