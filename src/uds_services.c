#include "uds.h"

#include <stddef.h>

uds_result_t uds_validate_diagnostic_session_control(
    const uds_request_t *request)
{
    if (request == NULL) {
        return UDS_ERROR_GENERAL_REJECT;
    }
    if (request->subfunction != 0x01u &&
        request->subfunction != 0x03u) {
        return UDS_ERROR_SUBFUNCTION_NOT_SUPPORTED;
    }
    return UDS_RESULT_OK;
}

uds_result_t uds_handle_diagnostic_session_control(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response)
{
    if (ctx == NULL || request == NULL || response == NULL ||
        response->capacity < 6u) {
        return UDS_ERROR_GENERAL_REJECT;
    }

    ctx->session = request->subfunction == 0x03u
                       ? UDS_SESSION_EXTENDED
                       : UDS_SESSION_DEFAULT;
    ctx->security_state = UDS_SECURITY_LOCKED;

    response->data[0] = UDS_SID_DIAGNOSTIC_SESSION_CONTROL +
                        UDS_POSITIVE_RESPONSE_OFFSET;
    response->data[1] = request->subfunction;
    response->data[2] = 0x00u;
    response->data[3] = 0x32u;
    response->data[4] = 0x01u;
    response->data[5] = 0xF4u;
    response->len = 6u;
    return UDS_RESULT_OK;
}

uds_result_t uds_handle_read_data_by_identifier(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response)
{
    uint16_t did;
    uint16_t data_len;
    uds_result_t result;

    if (ctx == NULL || request == NULL || response == NULL ||
        response->capacity < 3u) {
        return UDS_ERROR_GENERAL_REJECT;
    }

    did = ((uint16_t)request->data[1] << 8u) | request->data[2];
    response->data[0] = UDS_SID_READ_DATA_BY_IDENTIFIER +
                        UDS_POSITIVE_RESPONSE_OFFSET;
    response->data[1] = request->data[1];
    response->data[2] = request->data[2];
    result = uds_data_read_did(did, &response->data[3],
                               response->capacity - 3u, &data_len);
    if (result != UDS_RESULT_OK) {
        return result;
    }

    response->len = 3u + data_len;
    return UDS_RESULT_OK;
}
