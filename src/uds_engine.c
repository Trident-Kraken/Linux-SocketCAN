#define _POSIX_C_SOURCE 200809L

#include "uds.h"

#include <errno.h>
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
};

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
    return 0;
}

const uds_stats_t *uds_get_stats(const uds_ctx_t *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }
    return &ctx->stats;
}
