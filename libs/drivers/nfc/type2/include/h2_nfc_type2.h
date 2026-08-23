#ifndef H2_NFC_TYPE2_H
#define H2_NFC_TYPE2_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_NFC_TYPE2_NDEF_MAX_SIZE 1024u
#define H2_NFC_TYPE2_RESPONSE_MAX_SIZE 2040u

typedef struct h2_nfc_type2 h2_nfc_type2_t;

typedef struct h2_nfc_type2_config {
    const h2_pal_mem_api_t *mem;
    const uint8_t *uid;
    uint8_t uid_len;
    int enable_fast_read;
} h2_nfc_type2_config_t;

/** Create a read-only NFC Forum Type 2 protocol engine. */
h2_pal_result_t h2_nfc_type2_create(
    const h2_nfc_type2_config_t *config,
    h2_nfc_type2_t **out_type2);

/** Destroy an engine. NULL is accepted. */
void h2_nfc_type2_destroy(h2_nfc_type2_t *type2);

/**
 * Copy a complete NDEF message. During activation the revision is staged and
 * becomes visible atomically when the next activation begins.
 */
h2_pal_result_t h2_nfc_type2_set_ndef(
    h2_nfc_type2_t *type2,
    const uint8_t *ndef,
    size_t ndef_len,
    uint32_t revision);

/** Begin a new RF activation and publish any staged content revision. */
h2_pal_result_t h2_nfc_type2_activate(
    h2_nfc_type2_t *type2,
    uint32_t *out_revision);

/** End the current RF activation. */
h2_pal_result_t h2_nfc_type2_deactivate(h2_nfc_type2_t *type2);

/**
 * Process one ISO/IEC 14443-A or Type 2 command. A zero response length means
 * the command intentionally receives no response. This includes HLTA,
 * out-of-range reads, and write commands rejected by the read-only profile.
 */
h2_pal_result_t h2_nfc_type2_process(
    h2_nfc_type2_t *type2,
    const uint8_t *command,
    size_t command_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *out_response_len);

#ifdef __cplusplus
}
#endif

#endif
