#ifndef H2_PAL_NFC_H
#define H2_PAL_NFC_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/hal/h2_pal_periph.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_NFC_UID_MAX_LEN 16u
#define H2_PAL_NFC_CARD_EMULATION_UID_MAX_LEN 10u
#define H2_PAL_NFC_CARD_EMULATION_FRAME_MAX_SIZE 512u
#define H2_PAL_NFC_CARD_EMULATION_WAIT_FOREVER UINT32_MAX

typedef enum h2_pal_nfc_tag_type {
    H2_PAL_NFC_TAG_TYPE_UNKNOWN = 0,
    H2_PAL_NFC_TAG_TYPE_ISO14443A,
    H2_PAL_NFC_TAG_TYPE_MIFARE,
    H2_PAL_NFC_TAG_TYPE_NTAG,
} h2_pal_nfc_tag_type_t;

typedef enum h2_pal_nfc_stage {
    H2_PAL_NFC_STAGE_ABSENT = 0,
    H2_PAL_NFC_STAGE_DISCOVERED,
    H2_PAL_NFC_STAGE_ERROR,
} h2_pal_nfc_stage_t;

typedef enum h2_pal_nfc_data_type {
    H2_PAL_NFC_DATA_NONE = 0,
    H2_PAL_NFC_DATA_RAW,
    H2_PAL_NFC_DATA_NTAG_PAGES,
    H2_PAL_NFC_DATA_NDEF,
} h2_pal_nfc_data_type_t;

typedef struct h2_pal_nfc_scan {
    h2_pal_periph_id_t id;
    h2_pal_nfc_stage_t stage;
    h2_pal_result_t result;
    h2_pal_nfc_tag_type_t tag_type;
    uint8_t uid_len;
    uint8_t uid[H2_PAL_NFC_UID_MAX_LEN];
} h2_pal_nfc_scan_t;

typedef struct h2_pal_nfc_data_read {
    h2_pal_periph_id_t id;
    h2_pal_nfc_tag_type_t tag_type;
    uint8_t uid_len;
    uint8_t uid[H2_PAL_NFC_UID_MAX_LEN];
    h2_pal_nfc_data_type_t type;
    uint8_t *bytes;
    size_t len;
} h2_pal_nfc_data_read_t;

typedef struct h2_pal_nfc_vtable {
    h2_pal_result_t (*scan_nfc_reader)(
        void *user,
        h2_pal_periph_id_t id,
        h2_pal_nfc_scan_t *out_scan);

    h2_pal_result_t (*read_nfc_data)(
        void *user,
        h2_pal_periph_id_t id,
        const uint8_t *expected_uid,
        uint8_t expected_uid_len,
        h2_pal_nfc_data_type_t requested_type,
        const h2_pal_mem_api_t *allocator,
        h2_pal_nfc_data_read_t *out_data);
} h2_pal_nfc_vtable_t;

typedef struct h2_pal_nfc_api {
    void *user;
    const h2_pal_nfc_vtable_t *vtable;
} h2_pal_nfc_api_t;

static inline h2_pal_result_t h2_pal_nfc_scan_nfc_reader(
    const h2_pal_nfc_api_t *api,
    h2_pal_periph_id_t id,
    h2_pal_nfc_scan_t *out_scan) {
    if (out_scan == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->scan_nfc_reader == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->scan_nfc_reader(api->user, id, out_scan);
}

static inline h2_pal_result_t h2_pal_nfc_read_nfc_data(
    const h2_pal_nfc_api_t *api,
    h2_pal_periph_id_t id,
    const uint8_t *expected_uid,
    uint8_t expected_uid_len,
    h2_pal_nfc_data_type_t requested_type,
    const h2_pal_mem_api_t *allocator,
    h2_pal_nfc_data_read_t *out_data) {
    if (expected_uid_len > H2_PAL_NFC_UID_MAX_LEN ||
        (expected_uid == NULL && expected_uid_len != 0u) ||
        allocator == NULL || out_data == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->read_nfc_data == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->read_nfc_data(
        api->user,
        id,
        expected_uid,
        expected_uid_len,
        requested_type,
        allocator,
        out_data);
}

/** RF technologies supported by the independent card-emulation capability. */
typedef enum h2_pal_nfc_card_emulation_technology {
    H2_PAL_NFC_CARD_EMULATION_TECHNOLOGY_ISO14443A = 1u << 0,
} h2_pal_nfc_card_emulation_technology_t;

/** Managed card profiles implemented by a provider. */
typedef enum h2_pal_nfc_card_emulation_profile {
    H2_PAL_NFC_CARD_EMULATION_PROFILE_NONE = 0,
    H2_PAL_NFC_CARD_EMULATION_PROFILE_TYPE2_READ_ONLY = 1u << 0,
} h2_pal_nfc_card_emulation_profile_t;

/** Mutually exclusive exchange modes selected for one synchronous emulate call. */
typedef enum h2_pal_nfc_card_emulation_mode {
    H2_PAL_NFC_CARD_EMULATION_MODE_MANAGED_PROFILE = 1u << 0,
    H2_PAL_NFC_CARD_EMULATION_MODE_RAW_FRAME = 1u << 1,
} h2_pal_nfc_card_emulation_mode_t;

/** Provider framing ownership advertised for raw exchange mode. */
typedef struct h2_pal_nfc_card_emulation_raw_capabilities {
    size_t max_rx_frame_size;
    size_t max_tx_frame_size;
    uint32_t response_deadline_us;
    int supports_partial_bytes;
    int provider_owns_crc;
    int provider_owns_parity;
    int provider_owns_activation;
} h2_pal_nfc_card_emulation_raw_capabilities_t;

/** Capability snapshot for one physical NFC peripheral. */
typedef struct h2_pal_nfc_card_emulation_capabilities {
    uint32_t technology_mask;
    uint32_t managed_profile_mask;
    uint32_t exchange_mode_mask;
    uint8_t min_uid_len;
    uint8_t max_uid_len;
    size_t max_managed_content_len;
    h2_pal_nfc_card_emulation_raw_capabilities_t raw;
} h2_pal_nfc_card_emulation_capabilities_t;

/** A bit-precise frame view. Unused low bits in the final byte are zero. */
typedef struct h2_pal_nfc_card_emulation_frame {
    const uint8_t *bytes;
    size_t bit_len;
} h2_pal_nfc_card_emulation_frame_t;

/**
 * Raw exchange responder. Input is borrowed for the callback. The callback
 * writes a response into caller-provided storage, or returns WOULD_BLOCK to
 * request no response for this frame. It must not sleep or block and must
 * return within the provider's advertised response_deadline_us. The callback
 * and user pointer remain borrowed only until emulate returns.
 */
typedef h2_pal_result_t (*h2_pal_nfc_card_emulation_raw_exchange_fn)(
    void *user,
    const h2_pal_nfc_card_emulation_frame_t *request,
    uint8_t *response,
    size_t response_capacity,
    size_t *out_response_bit_len);

typedef struct h2_pal_nfc_card_emulation_config {
    h2_pal_periph_id_t periph_id;
    h2_pal_nfc_card_emulation_technology_t technology;
    h2_pal_nfc_card_emulation_mode_t mode;
    h2_pal_nfc_card_emulation_profile_t managed_profile;
    const uint8_t *uid;
    uint8_t uid_len;
    uint32_t content_revision;
    const uint8_t *managed_bytes;
    size_t managed_len;
    h2_pal_nfc_card_emulation_raw_exchange_fn raw_exchange;
    void *raw_exchange_user;
    uint32_t window_ms;
} h2_pal_nfc_card_emulation_config_t;

typedef enum h2_pal_nfc_card_emulation_completion {
    H2_PAL_NFC_CARD_EMULATION_COMPLETION_WINDOW_EXPIRED = 0,
    H2_PAL_NFC_CARD_EMULATION_COMPLETION_CONTENT_ACCESSED,
    H2_PAL_NFC_CARD_EMULATION_COMPLETION_DEACTIVATED,
} h2_pal_nfc_card_emulation_completion_t;

typedef struct h2_pal_nfc_card_emulation_result {
    h2_pal_nfc_card_emulation_completion_t completion;
    uint32_t content_revision;
    int field_detected;
    int activated;
} h2_pal_nfc_card_emulation_result_t;

typedef struct h2_pal_nfc_card_emulation_vtable {
    h2_pal_result_t (*get_capabilities)(
        void *user,
        h2_pal_periph_id_t periph_id,
        h2_pal_nfc_card_emulation_capabilities_t *out_capabilities);
    h2_pal_result_t (*emulate)(
        void *user,
        const h2_pal_nfc_card_emulation_config_t *config,
        h2_pal_nfc_card_emulation_result_t *out_result);
} h2_pal_nfc_card_emulation_vtable_t;

typedef struct h2_pal_nfc_card_emulation_api {
    void *user;
    const h2_pal_nfc_card_emulation_vtable_t *vtable;
} h2_pal_nfc_card_emulation_api_t;

static inline h2_pal_result_t h2_pal_nfc_card_emulation_get_capabilities(
    const h2_pal_nfc_card_emulation_api_t *api,
    h2_pal_periph_id_t periph_id,
    h2_pal_nfc_card_emulation_capabilities_t *out_capabilities) {
    if (out_capabilities == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->get_capabilities == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_capabilities(
        api->user, periph_id, out_capabilities);
}

static inline h2_pal_result_t h2_pal_nfc_card_emulate(
    const h2_pal_nfc_card_emulation_api_t *api,
    const h2_pal_nfc_card_emulation_config_t *config,
    h2_pal_nfc_card_emulation_result_t *out_result) {
    if (out_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_result, 0, sizeof(*out_result));
    if (config == NULL || config->uid == NULL || config->uid_len == 0u ||
        config->uid_len > H2_PAL_NFC_CARD_EMULATION_UID_MAX_LEN ||
        config->window_ms == 0u ||
        config->technology !=
            H2_PAL_NFC_CARD_EMULATION_TECHNOLOGY_ISO14443A ||
        (config->mode != H2_PAL_NFC_CARD_EMULATION_MODE_MANAGED_PROFILE &&
         config->mode != H2_PAL_NFC_CARD_EMULATION_MODE_RAW_FRAME) ||
        (config->mode == H2_PAL_NFC_CARD_EMULATION_MODE_MANAGED_PROFILE &&
         (config->managed_profile == H2_PAL_NFC_CARD_EMULATION_PROFILE_NONE ||
          (config->managed_bytes == NULL && config->managed_len != 0u) ||
          config->raw_exchange != NULL)) ||
        (config->mode == H2_PAL_NFC_CARD_EMULATION_MODE_RAW_FRAME &&
         (config->managed_profile != H2_PAL_NFC_CARD_EMULATION_PROFILE_NONE ||
          config->managed_bytes != NULL || config->managed_len != 0u ||
          config->raw_exchange == NULL))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->emulate == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->emulate(api->user, config, out_result);
}

#ifdef __cplusplus
}
#endif

#endif
