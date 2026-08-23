#ifndef H2_RUNTIME_INPUT_NFC_H
#define H2_RUNTIME_INPUT_NFC_H

/*
 * Scope: App-visible NFC event and state payloads.
 * Tunable macro defaults live in h2_runtime_input_nfc_defs.h.
 */

#include "h2_runtime_input_nfc_defs.h"
#include "h2_runtime_component.h"
#include "h2_runtime_input.h"
#include "h2/pal/hal/h2_pal_nfc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_runtime_nfc_state_status {
    H2_RUNTIME_NFC_STATE_NONE = 0,
    H2_RUNTIME_NFC_STATE_ABSENT,
    H2_RUNTIME_NFC_STATE_DISCOVERED,
    H2_RUNTIME_NFC_STATE_ERROR,
} h2_runtime_nfc_state_status_t;

typedef struct h2_runtime_nfc_state {
    h2_runtime_nfc_state_status_t status;
    h2_pal_nfc_stage_t stage;
    h2_pal_nfc_tag_type_t tag_type;
    uint8_t uid_len;
    uint8_t uid[H2_PAL_NFC_UID_MAX_LEN];
    h2_pal_result_t result;
    h2_runtime_timestamp_ms_t updated_at_ms;
} h2_runtime_nfc_state_t;

#ifdef __cplusplus
}
#endif

#endif
