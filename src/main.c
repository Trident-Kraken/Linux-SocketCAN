#define _POSIX_C_SOURCE 200809L

#include "can_driver.h"
#include "isotp.h"
#include "uds.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

#define MAIN_MAX_EVENTS (3 + ISOTP_TIMER_COUNT)
#define MAIN_CAN_RX_BUDGET 64u

static void main_log(const char *level, const char *format, ...)
{
    struct timespec now;
    struct tm local_time;
    char timestamp[32];
    va_list arguments;

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

    (void)fprintf(stderr, "[%s] [%s] [main] ", timestamp, level);
    va_start(arguments, format);
    (void)vfprintf(stderr, format, arguments);
    va_end(arguments);
    (void)fputc('\n', stderr);
}

static bool main_is_transient_tx_error(int error)
{
    return error == -EAGAIN || error == -EWOULDBLOCK || error == -ENOBUFS;
}

static int parse_can_id(const char *text, uint32_t *id)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || id == NULL || text[0] < '0' || text[0] > '9') {
        return -EINVAL;
    }

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed > CAN_STANDARD_ID_MAX) {
        return -EINVAL;
    }
    *id = (uint32_t)parsed;
    return 0;
}

static void print_usage(const char *program)
{
    (void)fprintf(stderr,
                  "Usage: %s [-i interface] [-r request_id] "
                  "[-t response_id] [-v]\n",
                  program);
}

static int open_signal_fd(void)
{
    sigset_t mask;
    int signal_fd;

    (void)sigemptyset(&mask);
    (void)sigaddset(&mask, SIGINT);
    (void)sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        return -errno;
    }
    signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd < 0) {
        return -errno;
    }
    return signal_fd;
}

static int main_drain_signals(int signal_fd, bool *stop_requested)
{
    for (;;) {
        struct signalfd_siginfo info;
        ssize_t bytes_read = read(signal_fd, &info, sizeof(info));

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            return -errno;
        }
        if ((size_t)bytes_read != sizeof(info)) {
            return -EIO;
        }
        if (info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM) {
            *stop_requested = true;
        }
    }
}

static int main_add_epoll_fd(int epoll_fd, int fd)
{
    struct epoll_event event = {0};

    if (fd < 0) {
        return -EINVAL;
    }
    event.events = EPOLLIN;
    event.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        return -errno;
    }
    return 0;
}

static int main_drain_can(can_driver_t *driver, isotp_ctx_t *isotp)
{
    for (unsigned int frames = 0u; frames < MAIN_CAN_RX_BUDGET; ++frames) {
        can_frame_t frame;
        int result = can_recv(driver, &frame);

        if (result == -EAGAIN) {
            return 0;
        }
        if (result < 0) {
            return result;
        }

        result = isotp_on_frame(isotp, &frame);
        if (result < 0) {
            if (main_is_transient_tx_error(result)) {
                main_log("WARN", "transient CAN TX error: %s",
                         strerror(-result));
                continue;
            }
            return result;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *interface_name = "vcan0";
    uint32_t request_id = 0x7E0u;
    uint32_t response_id = 0x7E8u;
    bool verbose = false;
    bool stop_requested = false;
    can_driver_t driver = {0};
    isotp_ctx_t isotp = {0};
    uds_ctx_t uds = {0};
    isotp_config_t isotp_config;
    struct epoll_event events[MAIN_MAX_EVENTS];
    const can_stats_t *can_stats;
    const isotp_stats_t *isotp_stats;
    const uds_stats_t *uds_stats;
    int epoll_fd = -1;
    int signal_fd = -1;
    int option;
    int result;
    int exit_status = EXIT_SUCCESS;

    while ((option = getopt(argc, argv, "i:r:t:vh")) != -1) {
        switch (option) {
        case 'i':
            interface_name = optarg;
            break;
        case 'r':
            result = parse_can_id(optarg, &request_id);
            if (result < 0) {
                (void)fprintf(stderr, "error: invalid request CAN ID: %s\n",
                              optarg);
                return EXIT_FAILURE;
            }
            break;
        case 't':
            result = parse_can_id(optarg, &response_id);
            if (result < 0) {
                (void)fprintf(stderr, "error: invalid response CAN ID: %s\n",
                              optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'v':
            verbose = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind < argc) {
        (void)fprintf(stderr, "error: unexpected argument: %s\n",
                      argv[optind]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (request_id == response_id) {
        (void)fprintf(stderr,
                      "error: request and response CAN IDs must differ: "
                      "0x%03X\n",
                      request_id);
        return EXIT_FAILURE;
    }

    signal_fd = open_signal_fd();
    if (signal_fd < 0) {
        (void)fprintf(stderr, "error: failed to create signalfd: %s\n",
                      strerror(-signal_fd));
        return EXIT_FAILURE;
    }

    result = can_open(&driver, interface_name, request_id, verbose);
    if (result < 0) {
        (void)fprintf(stderr, "error: cannot open CAN interface %s: %s\n",
                      interface_name, strerror(-result));
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    (void)memset(&isotp_config, 0, sizeof(isotp_config));
    isotp_config.frame_sender = can_driver_frame_sender(&driver);
    isotp_config.rx_id = request_id;
    isotp_config.tx_id = response_id;
    isotp_config.on_message = uds_on_isotp_message;
    isotp_config.on_tx_done = uds_on_isotp_tx_done;
    isotp_config.on_first_frame = uds_on_isotp_first_frame;
    isotp_config.on_rx_abort = uds_on_isotp_rx_abort;
    isotp_config.user = &uds;
    result = isotp_init(&isotp, &isotp_config);
    if (result < 0) {
        (void)fprintf(stderr, "error: ISO-TP initialization failed: %s\n",
                      strerror(-result));
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    result = uds_init(&uds, &isotp);
    if (result < 0) {
        (void)fprintf(stderr, "error: UDS initialization failed: %s\n",
                      strerror(-result));
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        (void)fprintf(stderr, "error: epoll_create1 failed: %s\n",
                      strerror(errno));
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    result = main_add_epoll_fd(epoll_fd, signal_fd);
    if (result < 0) {
        (void)fprintf(stderr, "error: cannot register signal fd: %s\n",
                      strerror(-result));
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }
    result = main_add_epoll_fd(epoll_fd, can_get_fd(&driver));
    if (result < 0) {
        (void)fprintf(stderr, "error: cannot register CAN fd: %s\n",
                      strerror(-result));
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }
    for (isotp_timer_t timer = ISOTP_TIMER_N_BS;
         timer < ISOTP_TIMER_COUNT; ++timer) {
        result = main_add_epoll_fd(epoll_fd,
                                   isotp_timer_fd(&isotp, timer));
        if (result < 0) {
            (void)fprintf(stderr,
                          "error: cannot register ISO-TP timer fd: %s\n",
                          strerror(-result));
            exit_status = EXIT_FAILURE;
            goto cleanup;
        }
    }
    result = main_add_epoll_fd(epoll_fd, uds_s3_timer_fd(&uds));
    if (result < 0) {
        (void)fprintf(stderr, "error: cannot register UDS timer fd: %s\n",
                      strerror(-result));
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    main_log("INFO", "ECU started on %s rx=0x%03X tx=0x%03X",
             interface_name, request_id, response_id);

    while (!stop_requested) {
        int ready = epoll_wait(epoll_fd, events, MAIN_MAX_EVENTS, -1);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            main_log("ERROR", "epoll_wait failed: %s", strerror(errno));
            exit_status = EXIT_FAILURE;
            break;
        }

        for (int index = 0; index < ready; ++index) {
            int ready_fd = events[index].data.fd;

            if (ready_fd == signal_fd) {
                result = main_drain_signals(signal_fd, &stop_requested);
            } else if (ready_fd == can_get_fd(&driver)) {
                result = main_drain_can(&driver, &isotp);
            } else if (ready_fd == uds_s3_timer_fd(&uds)) {
                result = uds_on_s3_timer(&uds);
            } else {
                result = 0;
                for (isotp_timer_t timer = ISOTP_TIMER_N_BS;
                     timer < ISOTP_TIMER_COUNT; ++timer) {
                    if (ready_fd == isotp_timer_fd(&isotp, timer)) {
                        result = isotp_on_timer(&isotp, timer);
                        if (main_is_transient_tx_error(result)) {
                            main_log("WARN", "transient CAN TX error: %s",
                                     strerror(-result));
                            result = 0;
                        }
                        break;
                    }
                }
            }

            if (result >= 0) {
                if (stop_requested) {
                    break;
                }
                continue;
            }
            main_log("ERROR", "event handling failed: %s",
                     strerror(-result));
            exit_status = EXIT_FAILURE;
            stop_requested = true;
            break;
        }
    }

    can_stats = can_get_stats(&driver);
    isotp_stats = isotp_get_stats(&isotp);
    uds_stats = uds_get_stats(&uds);
    if (can_stats != NULL && isotp_stats != NULL && uds_stats != NULL) {
        main_log("INFO",
                 "can_frames_rx=%llu can_frames_tx=%llu "
                 "uds_requests=%llu uds_positive=%llu uds_negative=%llu "
                 "isotp_timeouts=%llu isotp_sn_errors=%llu",
                 (unsigned long long)can_stats->frames_rx,
                 (unsigned long long)can_stats->frames_tx,
                 (unsigned long long)uds_stats->requests,
                 (unsigned long long)uds_stats->positive_responses,
                 (unsigned long long)uds_stats->negative_responses,
                 (unsigned long long)isotp_stats->timeouts,
                 (unsigned long long)isotp_stats->sn_errors);
    }

cleanup:
    uds_deinit(&uds);
    isotp_deinit(&isotp);
    can_close(&driver);
    if (epoll_fd >= 0) {
        (void)close(epoll_fd);
    }
    if (signal_fd >= 0) {
        (void)close(signal_fd);
    }
    return exit_status;
}
