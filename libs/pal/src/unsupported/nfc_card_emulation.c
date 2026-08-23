#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_nfc_card_emulation_get_capabilities(
    void *user,
    h2_pal_periph_id_t periph_id,
    h2_pal_nfc_card_emulation_capabilities_t *out_capabilities) {
    (void)user;
    (void)periph_id;
    (void)out_capabilities;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_nfc_card_emulation_emulate(
    void *user,
    const h2_pal_nfc_card_emulation_config_t *config,
    h2_pal_nfc_card_emulation_result_t *out_result) {
    (void)user;
    (void)config;
    (void)out_result;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_nfc_card_emulation_vtable_t
    unsupported_nfc_card_emulation_vtable = {
        .get_capabilities =
            unsupported_nfc_card_emulation_get_capabilities,
        .emulate = unsupported_nfc_card_emulation_emulate,
    };

static const h2_pal_nfc_card_emulation_api_t
    unsupported_nfc_card_emulation_api = {
        .user = NULL,
        .vtable = &unsupported_nfc_card_emulation_vtable,
    };

const h2_pal_nfc_card_emulation_api_t *
h2_pal_unsupported_nfc_card_emulation_api(void) {
    return &unsupported_nfc_card_emulation_api;
}
