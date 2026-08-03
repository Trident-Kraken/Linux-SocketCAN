#include "uds.h"

#include <stddef.h>
#include <string.h>

uint32_t uds_compute_key(uint32_t seed)
{
    uint32_t key = seed ^ 0xA5A5A5A5u;

    key = (key << 3u) | (key >> 29u);
    return key + 0x3C6EF35Fu;
}

typedef struct {
    uint16_t did;
    uint16_t len;
    bool     writable;
    uint8_t  data[17];
} uds_did_entry_t;

static uds_did_entry_t did_table[] = {
    {
        .did = UDS_DID_VIN,
        .len = 17u,
        .writable = true,
        .data = {0x4Cu, 0x56u, 0x55u, 0x44u, 0x53u, 0x32u, 0x30u,
                 0x32u, 0x36u, 0x44u, 0x45u, 0x4Du, 0x4Fu, 0x30u,
                 0x30u, 0x30u, 0x31u},
    },
    {
        .did = UDS_DID_ECU_SOFTWARE_VERSION,
        .len = 10u,
        .writable = false,
        .data = {0x76u, 0x55u, 0x44u, 0x53u, 0x2Du, 0x31u, 0x2Eu,
                 0x30u, 0x2Eu, 0x30u},
    },
    {
        .did = UDS_DID_COOLANT_TEMPERATURE,
        .len = 2u,
        .writable = false,
        .data = {0x00u, 0x55u},
    },
    {
        .did = UDS_DID_ENGINE_SPEED,
        .len = 2u,
        .writable = false,
        .data = {0x0Fu, 0xA0u},
    },
    {
        .did = UDS_DID_SAMPLE_PERIOD,
        .len = 2u,
        .writable = true,
        .data = {0x00u, 0x64u},
    },
};

typedef struct {
    uint32_t code;
    uint8_t  status;
} uds_dtc_entry_t;

static const uds_dtc_entry_t dtc_table[] = {
    {.code = 0x010203u, .status = 0x09u},
    {.code = 0x020304u, .status = 0x08u},
    {.code = 0x030401u, .status = 0x01u},
};

static uds_did_entry_t *uds_find_did(uint16_t did)
{
    for (size_t index = 0;
         index < sizeof(did_table) / sizeof(did_table[0]); ++index) {
        if (did_table[index].did == did) {
            return &did_table[index];
        }
    }
    return NULL;
}

uds_result_t uds_data_read_did(uint16_t did, uint8_t *data,
                               uint16_t capacity, uint16_t *len)
{
    if (data == NULL || len == NULL) {
        return UDS_ERROR_GENERAL_REJECT;
    }

    uds_did_entry_t *entry = uds_find_did(did);

    if (entry == NULL) {
        return UDS_ERROR_REQUEST_OUT_OF_RANGE;
    }
    if (capacity < entry->len) {
        return UDS_ERROR_GENERAL_REJECT;
    }

    (void)memcpy(data, entry->data, entry->len);
    *len = entry->len;
    return UDS_RESULT_OK;
}

uds_result_t uds_data_validate_write(uint16_t did, uint16_t len)
{
    uds_did_entry_t *entry = uds_find_did(did);

    if (entry == NULL || !entry->writable) {
        return UDS_ERROR_REQUEST_OUT_OF_RANGE;
    }
    if (entry->len != len) {
        return UDS_ERROR_INVALID_FORMAT;
    }

    return UDS_RESULT_OK;
}

uds_result_t uds_data_write_did(uint16_t did, const uint8_t *data,
                                uint16_t len)
{
    uds_did_entry_t *entry;
    uds_result_t result;

    if (data == NULL) {
        return UDS_ERROR_GENERAL_REJECT;
    }
    result = uds_data_validate_write(did, len);
    if (result != UDS_RESULT_OK) {
        return result;
    }
    entry = uds_find_did(did);
    (void)memcpy(entry->data, data, len);
    return UDS_RESULT_OK;
}

uds_result_t uds_data_read_dtcs(uint8_t status_mask, uint8_t *data,
                                uint16_t capacity, uint16_t *len)
{
    uint16_t offset = 0u;

    if (data == NULL || len == NULL) {
        return UDS_ERROR_GENERAL_REJECT;
    }
    for (size_t index = 0;
         index < sizeof(dtc_table) / sizeof(dtc_table[0]); ++index) {
        const uds_dtc_entry_t *entry = &dtc_table[index];

        if ((entry->status & status_mask) == 0u) {
            continue;
        }
        if ((uint16_t)(capacity - offset) < 4u) {
            return UDS_ERROR_GENERAL_REJECT;
        }
        data[offset] = (uint8_t)(entry->code >> 16u);
        data[offset + 1u] = (uint8_t)(entry->code >> 8u);
        data[offset + 2u] = (uint8_t)entry->code;
        data[offset + 3u] = entry->status;
        offset = (uint16_t)(offset + 4u);
    }
    *len = offset;
    return UDS_RESULT_OK;
}
