#include "h2/pal/hal/h2_pal_nfc.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>

typedef struct fake_provider {
    int calls;
} fake_provider_t;

static h2_pal_result_t fake_get_capabilities(
    void *user,
    h2_pal_periph_id_t periph_id,
    h2_pal_nfc_card_emulation_capabilities_t *out_capabilities) {
    fake_provider_t *provider = user;
    ++provider->calls;
    if (periph_id != 7u) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_capabilities = (h2_pal_nfc_card_emulation_capabilities_t){
        .technology_mask = H2_PAL_NFC_CARD_EMULATION_TECHNOLOGY_ISO14443A,
        .managed_profile_mask =
            H2_PAL_NFC_CARD_EMULATION_PROFILE_TYPE2_READ_ONLY,
        .exchange_mode_mask =
            H2_PAL_NFC_CARD_EMULATION_MODE_MANAGED_PROFILE |
            H2_PAL_NFC_CARD_EMULATION_MODE_RAW_FRAME,
        .min_uid_len = 4u,
        .max_uid_len = 7u,
        .max_managed_content_len = 512u,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t fake_emulate(
    void *user,
    const h2_pal_nfc_card_emulation_config_t *config,
    h2_pal_nfc_card_emulation_result_t *out_result) {
    fake_provider_t *provider = user;
    ++provider->calls;
    if (config->periph_id != 7u) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_result = (h2_pal_nfc_card_emulation_result_t){
        .completion =
            H2_PAL_NFC_CARD_EMULATION_COMPLETION_CONTENT_ACCESSED,
        .content_revision = config->content_revision,
        .field_detected = 1,
        .activated = 1,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t raw_exchange(
    void *user,
    const h2_pal_nfc_card_emulation_frame_t *request,
    uint8_t *response,
    size_t response_capacity,
    size_t *out_response_bit_len) {
    (void)user;
    (void)request;
    (void)response;
    (void)response_capacity;
    *out_response_bit_len = 0u;
    return H2_PAL_ERR_WOULD_BLOCK;
}

int main(void) {
    h2_pal_nfc_card_emulation_capabilities_t capabilities;
    h2_pal_nfc_card_emulation_result_t result;
    fake_provider_t provider = {0};
    const h2_pal_nfc_card_emulation_vtable_t vtable = {
        .get_capabilities = fake_get_capabilities,
        .emulate = fake_emulate,
    };
    const h2_pal_nfc_card_emulation_api_t api = {
        .user = &provider,
        .vtable = &vtable,
    };
    const uint8_t uid[] = {0x04u, 0x01u, 0x02u, 0x03u};
    const uint8_t ndef[] = {0xd1u, 0x01u, 0x00u, 0x54u};
    h2_pal_nfc_card_emulation_config_t config = {
        .periph_id = 7u,
        .technology = H2_PAL_NFC_CARD_EMULATION_TECHNOLOGY_ISO14443A,
        .mode = H2_PAL_NFC_CARD_EMULATION_MODE_MANAGED_PROFILE,
        .managed_profile =
            H2_PAL_NFC_CARD_EMULATION_PROFILE_TYPE2_READ_ONLY,
        .uid = uid,
        .uid_len = sizeof(uid),
        .content_revision = 9u,
        .managed_bytes = ndef,
        .managed_len = sizeof(ndef),
        .window_ms = 50u,
    };

    assert(h2_pal_nfc_card_emulation_get_capabilities(
               NULL, 7u, &capabilities) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_nfc_card_emulation_get_capabilities(
               h2_pal_unsupported_nfc_card_emulation_api(),
               7u, &capabilities) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_nfc_card_emulation_get_capabilities(
               &api, 7u, &capabilities) == H2_PAL_OK);
    assert(h2_pal_nfc_card_emulate(&api, &config, &result) == H2_PAL_OK);
    assert(result.completion ==
           H2_PAL_NFC_CARD_EMULATION_COMPLETION_CONTENT_ACCESSED);
    assert(result.content_revision == 9u && result.field_detected == 1 &&
           result.activated == 1);

    config.window_ms = 0u;
    result.field_detected = 7;
    assert(h2_pal_nfc_card_emulate(&api, &config, &result) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(result.field_detected == 0);
    config.window_ms = H2_PAL_NFC_CARD_EMULATION_WAIT_FOREVER;
    config.mode = H2_PAL_NFC_CARD_EMULATION_MODE_RAW_FRAME;
    config.managed_profile = H2_PAL_NFC_CARD_EMULATION_PROFILE_NONE;
    config.managed_bytes = NULL;
    config.managed_len = 0u;
    config.raw_exchange = raw_exchange;
    assert(h2_pal_nfc_card_emulate(&api, &config, &result) == H2_PAL_OK);
    assert(h2_pal_nfc_card_emulate(
               h2_pal_unsupported_nfc_card_emulation_api(),
               &config, &result) == H2_PAL_ERR_UNSUPPORTED);
    assert(provider.calls == 3);
    return 0;
}
