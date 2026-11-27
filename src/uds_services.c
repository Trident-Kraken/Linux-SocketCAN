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
        response->data == NULL || response->capacity < 6u) {
        return UDS_ERROR_GENERAL_REJECT;
    }

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

uds_result_t uds_validate_tester_present(const uds_request_t *request)
{
    if (request == NULL) {
        return UDS_ERROR_GENERAL_REJECT;
    }
    if (request->subfunction != 0x00u) {
        return UDS_ERROR_SUBFUNCTION_NOT_SUPPORTED;
    }
    return UDS_RESULT_OK;
}

uds_result_t uds_handle_tester_present(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response)
{
    if (ctx == NULL || request == NULL || response == NULL ||
        response->data == NULL || response->capacity < 2u) {
        return UDS_ERROR_GENERAL_REJECT;
    }

    response->data[0] = UDS_SID_TESTER_PRESENT + UDS_POSITIVE_RESPONSE_OFFSET;
    response->data[1] = request->subfunction;
    response->len = 2u;
    return UDS_RESULT_OK;
}

uds_result_t uds_validate_security_access(const uds_request_t *request)
{
    if (request == NULL) {
        return UDS_ERROR_GENERAL_REJECT;
    }
    if (request->subfunction == 0x01u) {
        return request->len == 2u ? UDS_RESULT_OK : UDS_ERROR_INVALID_FORMAT;
    }
    if (request->subfunction == 0x02u) {
        return request->len == 6u ? UDS_RESULT_OK : UDS_ERROR_INVALID_FORMAT;
    }
    return UDS_ERROR_SUBFUNCTION_NOT_SUPPORTED;
}

uds_result_t uds_validate_write_data_by_identifier(
    const uds_request_t *request)
{
    uint16_t did;

    if (request == NULL || request->data == NULL || request->len < 4u) {
        return UDS_ERROR_INVALID_FORMAT;
    }
    did = ((uint16_t)request->data[1] << 8u) | request->data[2];
    return uds_data_validate_write(did, (uint16_t)(request->len - 3u));
}

uds_result_t uds_handle_security_access(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response)
{
    if (ctx == NULL || request == NULL || response == NULL) {
        return UDS_ERROR_GENERAL_REJECT;
    }
    if (request->subfunction == 0x01u) {
        return uds_security_request_seed(ctx, response);
    }
    return uds_security_send_key(ctx, &request->data[2], response);
}

uds_result_t uds_validate_read_dtc_information(
    const uds_request_t *request)
{
    if (request == NULL) {
        return UDS_ERROR_GENERAL_REJECT;
    }
    if (request->subfunction != UDS_DTC_REPORT_BY_STATUS_MASK) {
        return UDS_ERROR_SUBFUNCTION_NOT_SUPPORTED;
    }
    return request->len == 3u ? UDS_RESULT_OK : UDS_ERROR_INVALID_FORMAT;
}

uds_result_t uds_handle_read_dtc_information(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response)
{
    uint16_t dtc_len;
    uds_result_t result;

    if (ctx == NULL || request == NULL || request->data == NULL ||
        response == NULL || response->data == NULL || response->capacity < 3u ||
        request->len < 3u) {
        return UDS_ERROR_GENERAL_REJECT;
    }

    response->data[0] = UDS_SID_READ_DTC_INFORMATION +
                        UDS_POSITIVE_RESPONSE_OFFSET;
    response->data[1] = UDS_DTC_REPORT_BY_STATUS_MASK;
    response->data[2] = UDS_DTC_STATUS_AVAILABILITY_MASK;
    result = uds_data_read_dtcs(request->data[2], &response->data[3],
                                (uint16_t)(response->capacity - 3u), &dtc_len);
    if (result != UDS_RESULT_OK) {
        return result;
    }
    response->len = (uint16_t)(3u + dtc_len);
    return UDS_RESULT_OK;
}

uds_result_t uds_handle_write_data_by_identifier(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response)
{
    uint16_t did;
    uds_result_t result;

    if (ctx == NULL || request == NULL || request->data == NULL ||
        response == NULL || response->data == NULL || response->capacity < 3u ||
        request->len < 4u) {
        return UDS_ERROR_GENERAL_REJECT;
    }

    did = ((uint16_t)request->data[1] << 8u) | request->data[2];
    result = uds_data_write_did(did, &request->data[3],
                                (uint16_t)(request->len - 3u));
    if (result != UDS_RESULT_OK) {
        return result;
    }

    response->data[0] = UDS_SID_WRITE_DATA_BY_IDENTIFIER +
                        UDS_POSITIVE_RESPONSE_OFFSET;
    response->data[1] = request->data[1];
    response->data[2] = request->data[2];
    response->len = 3u;
    return UDS_RESULT_OK;
}

uds_result_t uds_handle_read_data_by_identifier(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response)
{
    uint16_t did;
    uint16_t data_len;
    uds_result_t result;

    if (ctx == NULL || request == NULL || request->data == NULL ||
        response == NULL || response->data == NULL || response->capacity < 3u ||
        request->len < 3u) {
        return UDS_ERROR_GENERAL_REJECT;
    }

    did = ((uint16_t)request->data[1] << 8u) | request->data[2];
    response->data[0] = UDS_SID_READ_DATA_BY_IDENTIFIER +
                        UDS_POSITIVE_RESPONSE_OFFSET;
    response->data[1] = request->data[1];
    response->data[2] = request->data[2];
    result = uds_data_read_did(did, &response->data[3],
                               (uint16_t)(response->capacity - 3u), &data_len);
    if (result != UDS_RESULT_OK) {
        return result;
    }

    response->len = (uint16_t)(3u + data_len);
    return UDS_RESULT_OK;
}
