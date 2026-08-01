#include "uds.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    uint16_t did;
    uint16_t len;
    uint8_t  data[2];
} uds_did_entry_t;

static const uds_did_entry_t did_table[] = {
    {
        .did = UDS_DID_COOLANT_TEMPERATURE,
        .len = 2u,
        .data = {0x00u, 0x55u},
    },
};

uds_result_t uds_data_read_did(uint16_t did, uint8_t *data,
                               uint16_t capacity, uint16_t *len)
{
    if (data == NULL || len == NULL) {
        return UDS_ERROR_GENERAL_REJECT;
    }

    for (size_t index = 0;
         index < sizeof(did_table) / sizeof(did_table[0]); ++index) {
        if (did_table[index].did != did) {
            continue;
        }
        if (capacity < did_table[index].len) {
            return UDS_ERROR_GENERAL_REJECT;
        }

        (void)memcpy(data, did_table[index].data, did_table[index].len);
        *len = did_table[index].len;
        return UDS_RESULT_OK;
    }

    return UDS_ERROR_REQUEST_OUT_OF_RANGE;
}
