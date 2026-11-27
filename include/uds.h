#ifndef VUDS_UDS_H
#define VUDS_UDS_H

#include "isotp.h"

#include <stdbool.h>
#include <stdint.h>

#define UDS_SID_DIAGNOSTIC_SESSION_CONTROL 0x10u /* ISO 14229-1: 0x10 */
#define UDS_SID_READ_DTC_INFORMATION 0x19u       /* ISO 14229-1: 0x19 */
#define UDS_SID_TESTER_PRESENT 0x3Eu              /* ISO 14229-1: 0x3E */
#define UDS_SID_READ_DATA_BY_IDENTIFIER 0x22u    /* ISO 14229-1: 0x22 */
#define UDS_SID_WRITE_DATA_BY_IDENTIFIER 0x2Eu   /* ISO 14229-1: 0x2E */
#define UDS_SID_SECURITY_ACCESS 0x27u             /* ISO 14229-1: 0x27 */
#define UDS_POSITIVE_RESPONSE_OFFSET 0x40u       /* ISO 14229-1: positive SID */
#define UDS_NEGATIVE_RESPONSE_SID 0x7Fu          /* ISO 14229-1: negative SID */
#define UDS_S3_TIMEOUT_MS 5000u                   /* Project S3 requirement */
#define UDS_SECURITY_LOCKOUT_MS 10000u            /* Project anti-brute-force delay */
#define UDS_SECURITY_MAX_ATTEMPTS 3u              /* Project SecurityAccess limit */

#define UDS_NRC_GENERAL_REJECT 0x10u
#define UDS_NRC_SERVICE_NOT_SUPPORTED 0x11u
#define UDS_NRC_SUBFUNCTION_NOT_SUPPORTED 0x12u
#define UDS_NRC_INVALID_FORMAT 0x13u
#define UDS_NRC_BUSY_REPEAT_REQUEST 0x21u
#define UDS_NRC_REQUEST_SEQUENCE_ERROR 0x24u
#define UDS_NRC_REQUEST_OUT_OF_RANGE 0x31u
#define UDS_NRC_SECURITY_ACCESS_DENIED 0x33u
#define UDS_NRC_INVALID_KEY 0x35u
#define UDS_NRC_ATTEMPTS_EXCEEDED 0x36u
#define UDS_NRC_DELAY_NOT_EXPIRED 0x37u
#define UDS_NRC_SUBFUNCTION_NOT_SUPPORTED_IN_SESSION 0x7Eu
#define UDS_NRC_SERVICE_NOT_SUPPORTED_IN_SESSION 0x7Fu

#define UDS_SESSION_MASK_DEFAULT 0x01u
#define UDS_SESSION_MASK_EXTENDED 0x02u

#define UDS_DID_COOLANT_TEMPERATURE 0x0100u
#define UDS_DID_ENGINE_SPEED 0x0101u
#define UDS_DID_SAMPLE_PERIOD 0x0200u
#define UDS_DID_ECU_SOFTWARE_VERSION 0xF189u
#define UDS_DID_VIN 0xF190u

#define UDS_DTC_REPORT_BY_STATUS_MASK 0x02u
#define UDS_DTC_STATUS_AVAILABILITY_MASK 0x09u

typedef struct uds_ctx uds_ctx_t;

typedef enum {
    UDS_SESSION_DEFAULT,
    UDS_SESSION_EXTENDED,
} uds_session_t;

typedef enum {
    UDS_SECURITY_LOCKED,
    UDS_SECURITY_SEED_SENT,
    UDS_SECURITY_UNLOCKED,
} uds_security_state_t;

typedef struct {
    const uint8_t *data;
    uint16_t       len;
    uint8_t        sid;
    uint8_t        subfunction;
    bool           suppress_positive;
} uds_request_t;

typedef struct {
    uint8_t  *data;
    uint16_t  capacity;
    uint16_t  len;
} uds_response_t;

typedef enum {
    UDS_RESULT_OK = 0,
    UDS_ERROR_GENERAL_REJECT,
    UDS_ERROR_SERVICE_NOT_SUPPORTED,
    UDS_ERROR_SUBFUNCTION_NOT_SUPPORTED,
    UDS_ERROR_INVALID_FORMAT,
    UDS_ERROR_REQUEST_OUT_OF_RANGE,
    UDS_ERROR_SEQUENCE,
    UDS_ERROR_SECURITY_DENIED,
    UDS_ERROR_INVALID_KEY,
    UDS_ERROR_ATTEMPTS_EXCEEDED,
    UDS_ERROR_DELAY_NOT_EXPIRED,
    UDS_ERROR_SERVICE_NOT_SUPPORTED_IN_SESSION,
} uds_result_t;

typedef uds_result_t (*uds_validator_t)(const uds_request_t *request);
typedef uds_result_t (*uds_handler_t)(uds_ctx_t *ctx,
                                      const uds_request_t *request,
                                      uds_response_t *response);

typedef struct {
    uint8_t         sid;
    uint8_t         session_mask;
    bool            need_security;
    bool            has_subfunction;
    bool            supports_suppress;
    uint16_t        min_len;
    uint16_t        max_len;
    uds_validator_t validate;
    uds_handler_t   handle;
} uds_service_entry_t;

typedef enum {
    UDS_TX_NONE = 0,
    UDS_TX_POSITIVE,
    UDS_TX_NEGATIVE,
} uds_tx_kind_t;

typedef struct {
    uint64_t requests;
    uint64_t positive_responses;
    uint64_t negative_responses;
} uds_stats_t;

struct uds_ctx {
    isotp_ctx_t         *transport;
    uds_session_t        session;
    uds_security_state_t security_state;
    uint32_t             security_seed;
    uint64_t             security_lock_until_ns;
    uint8_t              security_failed_attempts;
    bool                 security_seed_valid;
    int                  s3_timer_fd;
    uint64_t             s3_saved_remaining_ns;
    uds_tx_kind_t        pending_tx_kind;
    bool                 s3_paused_for_rx;
    bool                 s3_paused_for_tx;
    uint8_t              response_buffer[ISOTP_MAX_LEN];
    uint8_t              busy_sid;
    bool                 busy_sid_valid;
    bool                 initialized;
    uds_stats_t          stats;
};

int                uds_init(uds_ctx_t *ctx, isotp_ctx_t *transport);
void               uds_deinit(uds_ctx_t *ctx);
void               uds_on_isotp_message(const uint8_t *data, uint16_t len,
                                         void *user);
void               uds_on_isotp_tx_done(int status, void *user);
void               uds_on_isotp_first_frame(void *user);
void               uds_on_isotp_rx_abort(void *user);
int                uds_s3_timer_fd(const uds_ctx_t *ctx);
int                uds_on_s3_timer(uds_ctx_t *ctx);
const uds_stats_t *uds_get_stats(const uds_ctx_t *ctx);

/* Internal interfaces shared by the UDS compilation units. */
uds_result_t uds_validate_diagnostic_session_control(
    const uds_request_t *request);
uds_result_t uds_handle_diagnostic_session_control(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response);
uds_result_t uds_validate_tester_present(const uds_request_t *request);
uds_result_t uds_handle_tester_present(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response);
uds_result_t uds_validate_security_access(const uds_request_t *request);
uds_result_t uds_handle_security_access(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response);
uds_result_t uds_security_request_seed(uds_ctx_t *ctx,
                                      uds_response_t *response);
uds_result_t uds_security_send_key(uds_ctx_t *ctx, const uint8_t *key,
                                   uds_response_t *response);
uds_result_t uds_validate_write_data_by_identifier(
    const uds_request_t *request);
uds_result_t uds_handle_write_data_by_identifier(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response);
uds_result_t uds_validate_read_dtc_information(
    const uds_request_t *request);
uds_result_t uds_handle_read_dtc_information(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response);
uds_result_t uds_handle_read_data_by_identifier(
    uds_ctx_t *ctx, const uds_request_t *request,
    uds_response_t *response);
uds_result_t uds_data_read_did(uint16_t did, uint8_t *data,
                               uint16_t capacity, uint16_t *len);
uds_result_t uds_data_validate_write(uint16_t did, uint16_t len);
uds_result_t uds_data_write_did(uint16_t did, const uint8_t *data,
                                uint16_t len);
uds_result_t uds_data_read_dtcs(uint8_t status_mask, uint8_t *data,
                                uint16_t capacity, uint16_t *len);
uint32_t     uds_compute_key(uint32_t seed);

#endif
