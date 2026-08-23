#define _POSIX_C_SOURCE 200809L

#include "uds.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static const uds_service_entry_t service_table[] = {
    {
        .sid = UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
        .session_mask = UDS_SESSION_MASK_DEFAULT |
                        UDS_SESSION_MASK_EXTENDED,
        .need_security = false,
        .has_subfunction = true,
        .supports_suppress = true,
        .min_len = 2u,
        .max_len = 2u,
        .validate = uds_validate_diagnostic_session_control,
        .handle = uds_handle_diagnostic_session_control,
    },
    {
        .sid = UDS_SID_READ_DTC_INFORMATION,
        .session_mask = UDS_SESSION_MASK_DEFAULT |
                        UDS_SESSION_MASK_EXTENDED,
        .need_security = false,
        .has_subfunction = true,
        .supports_suppress = false,
        .min_len = 3u,
        .max_len = 3u,
        .validate = uds_validate_read_dtc_information,
        .handle = uds_handle_read_dtc_information,
    },
    {
        .sid = UDS_SID_TESTER_PRESENT,
        .session_mask = UDS_SESSION_MASK_DEFAULT |
                        UDS_SESSION_MASK_EXTENDED,
        .need_security = false,
        .has_subfunction = true,
        .supports_suppress = true,
        .min_len = 2u,
        .max_len = 2u,
        .validate = uds_validate_tester_present,
        .handle = uds_handle_tester_present,
    },
    {
        .sid = UDS_SID_READ_DATA_BY_IDENTIFIER,
        .session_mask = UDS_SESSION_MASK_DEFAULT |
                        UDS_SESSION_MASK_EXTENDED,
        .need_security = false,
        .has_subfunction = false,
        .supports_suppress = false,
        .min_len = 3u,
        .max_len = 3u,
        .validate = NULL,
        .handle = uds_handle_read_data_by_identifier,
    },
    {
        .sid = UDS_SID_WRITE_DATA_BY_IDENTIFIER,
        .session_mask = UDS_SESSION_MASK_EXTENDED,
        .need_security = true,
        .has_subfunction = false,
        .supports_suppress = false,
        .min_len = 4u,
        .max_len = 20u,
        .validate = uds_validate_write_data_by_identifier,
        .handle = uds_handle_write_data_by_identifier,
    },
    {
        .sid = UDS_SID_SECURITY_ACCESS,
        .session_mask = UDS_SESSION_MASK_EXTENDED,
        .need_security = false,
        .has_subfunction = true,
        .supports_suppress = false,
        .min_len = 2u,
        .max_len = 6u,
        .validate = uds_validate_security_access,
        .handle = uds_handle_security_access,
    },
};

static uint64_t uds_monotonic_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL +
           (uint64_t)now.tv_nsec;
}

static bool uds_security_lock_active(uds_ctx_t *ctx)
{
    uint64_t now;

    if (ctx->security_lock_until_ns == 0u) {
        return false;
    }
    now = uds_monotonic_now_ns();
    if (now != 0u && now >= ctx->security_lock_until_ns) {
        ctx->security_lock_until_ns = 0u;
        ctx->security_failed_attempts = 0u;
        return false;
    }
    return true;
}

/*
 * SecurityAccess state transitions:
 * LOCKED --27 01--> SEED_SENT --correct key--> UNLOCKED
 * SEED_SENT --wrong key (1/2)--> SEED_SENT
 * SEED_SENT --wrong key (3)--> LOCKED + lockout deadline
 * session switch / S3 expiry --> LOCKED + invalidate seed
 */
static void uds_security_clear_seed(uds_ctx_t *ctx)
{
    ctx->security_seed = 0u;
    ctx->security_seed_valid = false;
    ctx->security_state = UDS_SECURITY_LOCKED;
}

static int uds_read_random_seed(uint32_t *seed)
{
    uint8_t bytes[sizeof(*seed)];
    int random_fd;
    size_t offset = 0u;

    if (seed == NULL) {
        return -EINVAL;
    }
    random_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random_fd < 0) {
        return -errno;
    }
    while (offset < sizeof(bytes)) {
        ssize_t count = read(random_fd, bytes + offset,
                             sizeof(bytes) - offset);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            int error = count == 0 ? EIO : errno;
            (void)close(random_fd);
            return -error;
        }
        offset += (size_t)count;
    }
    (void)close(random_fd);
    *seed = ((uint32_t)bytes[0] << 24u) |
            ((uint32_t)bytes[1] << 16u) |
            ((uint32_t)bytes[2] << 8u) |
            (uint32_t)bytes[3];
    return 0;
}

static bool uds_seed_is_degenerate(uint32_t seed)
{
    return seed == 0u || seed == UINT32_MAX;
}

static int uds_generate_seed(uint32_t *seed)
{
    for (unsigned int attempt = 0u; attempt < 16u; ++attempt) {
        int result = uds_read_random_seed(seed);

        if (result < 0) {
            return result;
        }
        if (!uds_seed_is_degenerate(*seed)) {
            return 0;
        }
    }
    return -EIO;
}

static uint32_t uds_read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24u) |
           ((uint32_t)data[1] << 16u) |
           ((uint32_t)data[2] << 8u) |
           (uint32_t)data[3];
}

static int uds_drain_s3_timer(uds_ctx_t *ctx)
{
    uint64_t expirations;
    ssize_t bytes_read;

    for (;;) {
        do {
            bytes_read = read(ctx->s3_timer_fd, &expirations,
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
    }
}

static int uds_stop_s3_timer(uds_ctx_t *ctx)
{
    const struct itimerspec disarmed = {0};

    if (timerfd_settime(ctx->s3_timer_fd, 0, &disarmed, NULL) < 0) {
        return -errno;
    }
    return uds_drain_s3_timer(ctx);
}

static int uds_start_s3_timer(uds_ctx_t *ctx)
{
    struct itimerspec value = {0};
    int result;

    result = uds_stop_s3_timer(ctx);
    if (result < 0) {
        return result;
    }
    value.it_value.tv_sec = UDS_S3_TIMEOUT_MS / 1000u;
    value.it_value.tv_nsec = (long)(UDS_S3_TIMEOUT_MS % 1000u) * 1000000L;
    if (timerfd_settime(ctx->s3_timer_fd, 0, &value, NULL) < 0) {
        return -errno;
    }
    return 0;
}

static int uds_refresh_s3(uds_ctx_t *ctx)
{
    if (ctx->session == UDS_SESSION_EXTENDED) {
        return uds_start_s3_timer(ctx);
    }
    return uds_stop_s3_timer(ctx);
}

static int uds_apply_session_control(uds_ctx_t *ctx, uint8_t subfunction)
{
    int result;

    if (subfunction == 0x03u) {
        result = uds_start_s3_timer(ctx);
        if (result < 0) {
            return result;
        }
        ctx->session = UDS_SESSION_EXTENDED;
    } else {
        result = uds_stop_s3_timer(ctx);
        if (result < 0) {
            return result;
        }
        ctx->session = UDS_SESSION_DEFAULT;
    }
    uds_security_clear_seed(ctx);
    return 0;
}

uds_result_t uds_security_request_seed(uds_ctx_t *ctx,
                                      uds_response_t *response)
{
    uint32_t seed;
    int result;

    if (ctx == NULL || response == NULL || response->data == NULL ||
        response->capacity < 6u) {
        return UDS_ERROR_GENERAL_REJECT;
    }
    if (uds_security_lock_active(ctx)) {
        return UDS_ERROR_DELAY_NOT_EXPIRED;
    }
    result = uds_generate_seed(&seed);
    if (result < 0) {
        return UDS_ERROR_GENERAL_REJECT;
    }

    ctx->security_seed = seed;
    ctx->security_seed_valid = true;
    ctx->security_state = UDS_SECURITY_SEED_SENT;
    response->data[0] = UDS_SID_SECURITY_ACCESS +
                        UDS_POSITIVE_RESPONSE_OFFSET;
    response->data[1] = 0x01u;
    response->data[2] = (uint8_t)(seed >> 24u);
    response->data[3] = (uint8_t)(seed >> 16u);
    response->data[4] = (uint8_t)(seed >> 8u);
    response->data[5] = (uint8_t)seed;
    response->len = 6u;
    return UDS_RESULT_OK;
}

uds_result_t uds_security_send_key(uds_ctx_t *ctx, const uint8_t *key,
                                   uds_response_t *response)
{
    uint32_t provided_key;
    uint32_t expected_key;

    if (ctx == NULL || key == NULL || response == NULL ||
        response->data == NULL || response->capacity < 2u) {
        return UDS_ERROR_GENERAL_REJECT;
    }
    if (!ctx->security_seed_valid ||
        ctx->security_state != UDS_SECURITY_SEED_SENT) {
        return UDS_ERROR_SEQUENCE;
    }

    provided_key = uds_read_be32(key);
    expected_key = uds_compute_key(ctx->security_seed);
    if (provided_key != expected_key) {
        if (ctx->security_failed_attempts < UDS_SECURITY_MAX_ATTEMPTS) {
            ctx->security_failed_attempts++;
        }
        if (ctx->security_failed_attempts >= UDS_SECURITY_MAX_ATTEMPTS) {
            uint64_t now = uds_monotonic_now_ns();

            uds_security_clear_seed(ctx);
            ctx->security_lock_until_ns = now == 0u
                                              ? UINT64_MAX
                                              : now +
                                                    (uint64_t)UDS_SECURITY_LOCKOUT_MS *
                                                        1000000ULL;
            return UDS_ERROR_ATTEMPTS_EXCEEDED;
        }
        return UDS_ERROR_INVALID_KEY;
    }

    uds_security_clear_seed(ctx);
    ctx->security_state = UDS_SECURITY_UNLOCKED;
    ctx->security_failed_attempts = 0u;
    ctx->security_lock_until_ns = 0u;
    response->data[0] = UDS_SID_SECURITY_ACCESS +
                        UDS_POSITIVE_RESPONSE_OFFSET;
    response->data[1] = 0x02u;
    response->len = 2u;
    return UDS_RESULT_OK;
}

static uint8_t uds_session_mask(uds_session_t session)
{
    return session == UDS_SESSION_EXTENDED
               ? UDS_SESSION_MASK_EXTENDED
               : UDS_SESSION_MASK_DEFAULT;
}

static const uds_service_entry_t *uds_find_service(uint8_t sid)
{
    for (size_t index = 0;
         index < sizeof(service_table) / sizeof(service_table[0]); ++index) {
        if (service_table[index].sid == sid) {
            return &service_table[index];
        }
    }
    return NULL;
}

static uint8_t uds_result_to_nrc(uds_result_t result)
{
    switch (result) {
    case UDS_ERROR_SERVICE_NOT_SUPPORTED:
        return UDS_NRC_SERVICE_NOT_SUPPORTED;
    case UDS_ERROR_SUBFUNCTION_NOT_SUPPORTED:
        return UDS_NRC_SUBFUNCTION_NOT_SUPPORTED;
    case UDS_ERROR_INVALID_FORMAT:
        return UDS_NRC_INVALID_FORMAT;
    case UDS_ERROR_REQUEST_OUT_OF_RANGE:
        return UDS_NRC_REQUEST_OUT_OF_RANGE;
    case UDS_ERROR_SEQUENCE:
        return UDS_NRC_REQUEST_SEQUENCE_ERROR;
    case UDS_ERROR_SECURITY_DENIED:
        return UDS_NRC_SECURITY_ACCESS_DENIED;
    case UDS_ERROR_INVALID_KEY:
        return UDS_NRC_INVALID_KEY;
    case UDS_ERROR_ATTEMPTS_EXCEEDED:
        return UDS_NRC_ATTEMPTS_EXCEEDED;
    case UDS_ERROR_DELAY_NOT_EXPIRED:
        return UDS_NRC_DELAY_NOT_EXPIRED;
    case UDS_ERROR_SERVICE_NOT_SUPPORTED_IN_SESSION:
        return UDS_NRC_SERVICE_NOT_SUPPORTED_IN_SESSION;
    case UDS_RESULT_OK:
    case UDS_ERROR_GENERAL_REJECT:
    default:
        return UDS_NRC_GENERAL_REJECT;
    }
}

static void uds_send_negative_nrc(uds_ctx_t *ctx, uint8_t sid, uint8_t nrc)
{
    uint8_t response[3] = {
        UDS_NEGATIVE_RESPONSE_SID,
        sid,
        nrc,
    };

    if (isotp_send(ctx->transport, response, sizeof(response)) == 0) {
        ctx->stats.negative_responses++;
    }
}

static void uds_send_negative(uds_ctx_t *ctx, uint8_t sid,
                              uds_result_t result)
{
    uds_send_negative_nrc(ctx, sid, uds_result_to_nrc(result));
}

int uds_init(uds_ctx_t *ctx, isotp_ctx_t *transport)
{
    int timer_fd;

    if (ctx == NULL || transport == NULL) {
        return -EINVAL;
    }
    if (ctx->initialized) {
        return -EBUSY;
    }

    timer_fd = timerfd_create(CLOCK_MONOTONIC,
                              TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        return -errno;
    }

    (void)memset(ctx, 0, sizeof(*ctx));
    ctx->transport = transport;
    ctx->session = UDS_SESSION_DEFAULT;
    ctx->security_state = UDS_SECURITY_LOCKED;
    ctx->s3_timer_fd = timer_fd;
    ctx->initialized = true;
    return 0;
}

void uds_deinit(uds_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->initialized) {
        return;
    }

    if (ctx->s3_timer_fd >= 0) {
        (void)uds_stop_s3_timer(ctx);
        (void)close(ctx->s3_timer_fd);
    }
    (void)memset(ctx, 0, sizeof(*ctx));
}

void uds_on_isotp_message(const uint8_t *data, uint16_t len, void *user)
{
    uds_ctx_t *ctx = user;
    const uds_service_entry_t *service;
    uds_request_t request;
    uds_response_t response;
    uds_result_t result;

    if (ctx == NULL || !ctx->initialized || data == NULL || len == 0u) {
        return;
    }

    ctx->stats.requests++;
    if (isotp_tx_busy(ctx->transport)) {
        if (!ctx->busy_sid_valid) {
            ctx->busy_sid = data[0];
            ctx->busy_sid_valid = true;
        }
        return;
    }

    service = uds_find_service(data[0]);
    if (service == NULL) {
        uds_send_negative(ctx, data[0], UDS_ERROR_SERVICE_NOT_SUPPORTED);
        return;
    }
    if (len < service->min_len || len > service->max_len) {
        uds_send_negative(ctx, data[0], UDS_ERROR_INVALID_FORMAT);
        return;
    }
    if ((service->session_mask & uds_session_mask(ctx->session)) == 0u) {
        uds_send_negative(ctx, data[0],
                          UDS_ERROR_SERVICE_NOT_SUPPORTED_IN_SESSION);
        return;
    }

    request.data = data;
    request.len = len;
    request.sid = data[0];
    request.subfunction = service->has_subfunction ? data[1] : 0u;
    request.suppress_positive = false;
    if (service->supports_suppress) {
        request.suppress_positive =
            (request.subfunction & 0x80u) != 0u;
        request.subfunction &= 0x7Fu;
    }

    result = service->validate != NULL
                 ? service->validate(&request)
                 : UDS_RESULT_OK;
    if (result != UDS_RESULT_OK) {
        uds_send_negative(ctx, request.sid, result);
        return;
    }
    if (service->sid != UDS_SID_DIAGNOSTIC_SESSION_CONTROL) {
        if (uds_refresh_s3(ctx) < 0) {
            uds_send_negative(ctx, request.sid, UDS_ERROR_GENERAL_REJECT);
            return;
        }
    }
    if (service->need_security &&
        ctx->security_state != UDS_SECURITY_UNLOCKED) {
        uds_send_negative(ctx, request.sid, UDS_ERROR_SECURITY_DENIED);
        return;
    }

    response.data = ctx->response_buffer;
    response.capacity = sizeof(ctx->response_buffer);
    response.len = 0u;
    result = service->handle(ctx, &request, &response);
    if (result != UDS_RESULT_OK) {
        uds_send_negative(ctx, request.sid, result);
        return;
    }
    if (service->sid == UDS_SID_DIAGNOSTIC_SESSION_CONTROL &&
        uds_apply_session_control(ctx, request.subfunction) < 0) {
        uds_send_negative(ctx, request.sid, UDS_ERROR_GENERAL_REJECT);
        return;
    }
    if (request.suppress_positive) {
        return;
    }
    if (isotp_send(ctx->transport, response.data, response.len) == 0) {
        ctx->stats.positive_responses++;
    }
}

void uds_on_isotp_tx_done(int status, void *user)
{
    uds_ctx_t *ctx = user;
    uint8_t busy_sid;

    (void)status;
    if (ctx == NULL || !ctx->initialized || !ctx->busy_sid_valid) {
        return;
    }

    busy_sid = ctx->busy_sid;
    ctx->busy_sid = 0u;
    ctx->busy_sid_valid = false;
    uds_send_negative_nrc(ctx, busy_sid, UDS_NRC_BUSY_REPEAT_REQUEST);
}

int uds_s3_timer_fd(const uds_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->initialized) {
        return -1;
    }
    return ctx->s3_timer_fd;
}

int uds_on_s3_timer(uds_ctx_t *ctx)
{
    uint64_t expirations;
    ssize_t bytes_read;

    if (ctx == NULL || !ctx->initialized) {
        return -EINVAL;
    }

    do {
        bytes_read = read(ctx->s3_timer_fd, &expirations,
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
    /* A successful timerfd read always reports at least one expiration. */
    ctx->session = UDS_SESSION_DEFAULT;
    uds_security_clear_seed(ctx);
    return uds_stop_s3_timer(ctx);
}

const uds_stats_t *uds_get_stats(const uds_ctx_t *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }
    return &ctx->stats;
}
