#include "h2_nfc_type2.h"

#include <string.h>

#define TYPE2_STATIC_MEMORY_MIN_SIZE 64u
#define TYPE2_CASCADE_TAG 0x88u

struct h2_nfc_type2 {
    const h2_pal_mem_api_t *mem;
    uint8_t uid[7];
    uint8_t uid_len;
    int enable_fast_read;
    int active;
    int halted;
    int has_staged;
    uint8_t *current_ndef;
    size_t current_ndef_len;
    uint32_t current_revision;
    uint8_t *staged_ndef;
    size_t staged_ndef_len;
    uint32_t staged_revision;
    size_t memory_len;
};

static uint8_t type2_bcc(const uint8_t *bytes, size_t len) {
    uint8_t value = 0u;
    for (size_t index = 0u; index < len; ++index) {
        value ^= bytes[index];
    }
    return value;
}

static void type2_update_memory_len(h2_nfc_type2_t *type2) {
    size_t tlv_header_len = type2->current_ndef_len < 0xffu ? 2u : 4u;
    size_t data_len = tlv_header_len + type2->current_ndef_len + 1u;
    size_t memory_len = 16u + data_len;
    if (memory_len < TYPE2_STATIC_MEMORY_MIN_SIZE) {
        memory_len = TYPE2_STATIC_MEMORY_MIN_SIZE;
    }
    memory_len = (memory_len + 7u) & ~(size_t)7u;
    type2->memory_len = memory_len;
}

static uint8_t type2_memory_byte(
    const h2_nfc_type2_t *type2,
    size_t offset) {
    if (offset < 3u) {
        return type2->uid[offset];
    }
    if (offset == 3u) {
        if (type2->uid_len == 4u) {
            return type2_bcc(type2->uid, 4u);
        }
        const uint8_t cascade[] = {
            TYPE2_CASCADE_TAG,
            type2->uid[0],
            type2->uid[1],
            type2->uid[2],
        };
        return type2_bcc(cascade, sizeof(cascade));
    }
    if (offset >= 4u && offset <= 7u) {
        return type2->uid_len == 4u
                   ? (offset == 4u ? type2->uid[3] : 0u)
                   : type2->uid[offset - 1u];
    }
    if (offset == 8u && type2->uid_len == 7u) {
        return type2_bcc(&type2->uid[3], 4u);
    }
    if (offset == 9u) {
        return 0x48u;
    }
    if (offset == 12u) {
        return 0xe1u;
    }
    if (offset == 13u) {
        return 0x10u;
    }
    if (offset == 14u) {
        return (uint8_t)((type2->memory_len - 16u) / 8u);
    }
    if (offset == 15u) {
        return 0x0fu;
    }
    if (offset == 16u) {
        return 0x03u;
    }

    const size_t ndef_offset = type2->current_ndef_len < 0xffu ? 18u : 20u;
    if (offset == 17u) {
        return type2->current_ndef_len < 0xffu
                   ? (uint8_t)type2->current_ndef_len
                   : 0xffu;
    }
    if (ndef_offset == 20u && offset == 18u) {
        return (uint8_t)(type2->current_ndef_len >> 8u);
    }
    if (ndef_offset == 20u && offset == 19u) {
        return (uint8_t)type2->current_ndef_len;
    }
    if (offset >= ndef_offset &&
        offset - ndef_offset < type2->current_ndef_len) {
        return type2->current_ndef[offset - ndef_offset];
    }
    if (offset == ndef_offset + type2->current_ndef_len) {
        return 0xfeu;
    }
    return 0u;
}

static void type2_read_memory(
    const h2_nfc_type2_t *type2,
    size_t offset,
    uint8_t *response,
    size_t response_len) {
    for (size_t index = 0u; index < response_len; ++index) {
        response[index] = type2_memory_byte(type2, offset + index);
    }
}

static h2_pal_result_t type2_replace_ndef(
    h2_nfc_type2_t *type2,
    uint8_t **target,
    size_t *target_len,
    const uint8_t *ndef,
    size_t ndef_len) {
    uint8_t *replacement = NULL;
    if (ndef_len != 0u) {
        replacement = h2_pal_mem_alloc(type2->mem, ndef_len);
        if (replacement == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(replacement, ndef, ndef_len);
    }
    h2_pal_mem_free(type2->mem, *target);
    *target = replacement;
    *target_len = ndef_len;
    return H2_PAL_OK;
}

h2_pal_result_t h2_nfc_type2_create(
    const h2_nfc_type2_config_t *config,
    h2_nfc_type2_t **out_type2) {
    if (out_type2 == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_type2 = NULL;
    if (config == NULL || config->mem == NULL || config->uid == NULL ||
        (config->uid_len != 4u && config->uid_len != 7u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_nfc_type2_t *type2 = h2_pal_mem_alloc(config->mem, sizeof(*type2));
    if (type2 == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(type2, 0, sizeof(*type2));
    type2->mem = config->mem;
    memcpy(type2->uid, config->uid, config->uid_len);
    type2->uid_len = config->uid_len;
    type2->enable_fast_read = config->enable_fast_read != 0;
    type2_update_memory_len(type2);
    *out_type2 = type2;
    return H2_PAL_OK;
}

void h2_nfc_type2_destroy(h2_nfc_type2_t *type2) {
    if (type2 == NULL) {
        return;
    }
    const h2_pal_mem_api_t *mem = type2->mem;
    h2_pal_mem_free(mem, type2->current_ndef);
    h2_pal_mem_free(mem, type2->staged_ndef);
    memset(type2, 0, sizeof(*type2));
    h2_pal_mem_free(mem, type2);
}

h2_pal_result_t h2_nfc_type2_set_ndef(
    h2_nfc_type2_t *type2,
    const uint8_t *ndef,
    size_t ndef_len,
    uint32_t revision) {
    if (type2 == NULL || (ndef == NULL && ndef_len != 0u) ||
        ndef_len > H2_NFC_TYPE2_NDEF_MAX_SIZE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (type2->active) {
        h2_pal_result_t result = type2_replace_ndef(
            type2, &type2->staged_ndef, &type2->staged_ndef_len,
            ndef, ndef_len);
        if (result != H2_PAL_OK) {
            return result;
        }
        type2->staged_revision = revision;
        type2->has_staged = 1;
        return H2_PAL_OK;
    }
    h2_pal_result_t result = type2_replace_ndef(
        type2, &type2->current_ndef, &type2->current_ndef_len,
        ndef, ndef_len);
    if (result != H2_PAL_OK) {
        return result;
    }
    type2->current_revision = revision;
    type2_update_memory_len(type2);
    return H2_PAL_OK;
}

h2_pal_result_t h2_nfc_type2_activate(
    h2_nfc_type2_t *type2,
    uint32_t *out_revision) {
    if (type2 == NULL || out_revision == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (type2->active) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (type2->has_staged) {
        h2_pal_mem_free(type2->mem, type2->current_ndef);
        type2->current_ndef = type2->staged_ndef;
        type2->current_ndef_len = type2->staged_ndef_len;
        type2->current_revision = type2->staged_revision;
        type2->staged_ndef = NULL;
        type2->staged_ndef_len = 0u;
        type2->has_staged = 0;
        type2_update_memory_len(type2);
    }
    type2->active = 1;
    type2->halted = 0;
    *out_revision = type2->current_revision;
    return H2_PAL_OK;
}

h2_pal_result_t h2_nfc_type2_deactivate(h2_nfc_type2_t *type2) {
    if (type2 == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!type2->active) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    type2->active = 0;
    type2->halted = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t type2_copy_response(
    const uint8_t *source,
    size_t source_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *out_response_len) {
    if (source_len > response_capacity) {
        return H2_PAL_ERR_TRUNCATED;
    }
    memcpy(response, source, source_len);
    *out_response_len = source_len;
    return H2_PAL_OK;
}

h2_pal_result_t h2_nfc_type2_process(
    h2_nfc_type2_t *type2,
    const uint8_t *command,
    size_t command_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *out_response_len) {
    if (type2 == NULL || command == NULL || command_len == 0u ||
        response == NULL || out_response_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_response_len = 0u;
    if (!type2->active) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    if ((command[0] == 0x26u || command[0] == 0x52u) && command_len == 1u) {
        if (type2->halted && command[0] == 0x26u) {
            return H2_PAL_ERR_WOULD_BLOCK;
        }
        type2->halted = 0;
        const uint8_t atqa[] = {
            type2->uid_len == 7u ? 0x44u : 0x04u,
            0x00u,
        };
        return type2_copy_response(
            atqa, sizeof(atqa), response, response_capacity, out_response_len);
    }
    if (type2->halted) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (command[0] == 0x50u) {
        if (command_len != 2u || command[1] != 0x00u) {
            return H2_PAL_ERR_FORMAT;
        }
        type2->halted = 1;
        return H2_PAL_OK;
    }
    if ((command[0] == 0x93u || command[0] == 0x95u) && command_len >= 2u) {
        uint8_t cascade[5];
        uint8_t sak;
        if (command[0] == 0x93u) {
            if (type2->uid_len == 7u) {
                cascade[0] = TYPE2_CASCADE_TAG;
                memcpy(&cascade[1], type2->uid, 3u);
                cascade[4] = type2_bcc(cascade, 4u);
                sak = 0x04u;
            } else {
                memcpy(cascade, type2->uid, 4u);
                cascade[4] = type2_bcc(cascade, 4u);
                sak = 0x00u;
            }
        } else {
            if (type2->uid_len != 7u) {
                return H2_PAL_ERR_FORMAT;
            }
            memcpy(cascade, &type2->uid[3], 4u);
            cascade[4] = type2_bcc(cascade, 4u);
            sak = 0x00u;
        }
        if (command[1] == 0x20u && command_len == 2u) {
            return type2_copy_response(
                cascade, sizeof(cascade), response, response_capacity,
                out_response_len);
        }
        if (command[1] == 0x70u && command_len == 7u &&
            memcmp(&command[2], cascade, sizeof(cascade)) == 0) {
            return type2_copy_response(
                &sak, 1u, response, response_capacity, out_response_len);
        }
        return H2_PAL_ERR_FORMAT;
    }
    if (command[0] == 0x30u) {
        if (command_len != 2u) {
            return H2_PAL_ERR_FORMAT;
        }
        size_t offset = (size_t)command[1] * 4u;
        if (offset > type2->memory_len || type2->memory_len - offset < 16u) {
            return H2_PAL_OK;
        }
        if (response_capacity < 16u) {
            return H2_PAL_ERR_TRUNCATED;
        }
        type2_read_memory(type2, offset, response, 16u);
        *out_response_len = 16u;
        return H2_PAL_OK;
    }
    if (command[0] == 0x3au) {
        if (!type2->enable_fast_read) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
        if (command_len != 3u || command[2] < command[1]) {
            return H2_PAL_ERR_FORMAT;
        }
        size_t offset = (size_t)command[1] * 4u;
        size_t response_len = ((size_t)command[2] - command[1] + 1u) * 4u;
        if (offset > type2->memory_len ||
            response_len > type2->memory_len - offset) {
            return H2_PAL_OK;
        }
        if (response_capacity < response_len) {
            return H2_PAL_ERR_TRUNCATED;
        }
        type2_read_memory(type2, offset, response, response_len);
        *out_response_len = response_len;
        return H2_PAL_OK;
    }
    if (command[0] == 0xa2u || command[0] == 0xa0u) {
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}
