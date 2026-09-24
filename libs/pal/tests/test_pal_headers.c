#include "h2_pal.h"

#include <assert.h>
#include <string.h>

static void use_firmware_info(void) {
    h2_pal_firmware_info_t info = { 0 };
    (void)h2_pal_firmware_info_get_current(NULL, &info);
}

static void use_video_decoder(void) {
    h2_pal_video_decoder_session_t *session = NULL;
    const h2_video_decoder_config_t config = {0};
    (void)h2_pal_video_decoder_open(NULL, &config, &session);
}

static void use_audio_decoder(void) {
    h2_pal_audio_decoder_session_t *session = NULL;
    const h2_audio_decoder_config_t config = {0};
    (void)h2_pal_audio_decoder_open(NULL, &config, &session);
}

static void use_audio_task_names(void) {
    assert(strcmp(H2_PAL_AUDIO_MIC_TASK_NAME_VALUE, "$audio/mic") == 0);
    assert(strcmp(H2_PAL_AUDIO_MIX_TASK_NAME_VALUE, "$audio/mix") == 0);
}

static void use_wifi_csi(void) {
    h2_pal_wifi_csi_capabilities_t capabilities = {0};
    (void)h2_pal_wifi_csi_get_capabilities(NULL, &capabilities);
}

static void use_serial_host(void) {
    h2_pal_serial_host_snapshot_t *snapshot = NULL;
    (void)h2_pal_serial_host_scan(NULL, &snapshot);
    (void)h2_pal_serial_host_snapshot_destroy(NULL, &snapshot);
}

static void use_crypto(void) {
    h2_pal_p256_private_key_t private_key = {0};
    h2_pal_p256_public_key_t public_key = {0};
    h2_pal_p256_keypair_t keypair = {0};
    h2_pal_p256_signature_t signature = {0};
    (void)h2_pal_crypto_p256_keypair_from_private(
        NULL, &private_key, &keypair);
    (void)h2_pal_crypto_p256_keypair_generate(NULL, &keypair);
    (void)h2_pal_crypto_p256_public_key_validate(NULL, &public_key);
    (void)h2_pal_crypto_ecdsa_p256_sha256_sign(
        NULL, &private_key, NULL, 0u, &signature);
    (void)h2_pal_crypto_ecdsa_p256_sha256_verify(
        NULL, &public_key, NULL, 0u, &signature);
}

static void use_dtls(void) {
    h2_pal_dtls_session_t *session = NULL;
    h2_pal_dtls_session_destroy(NULL, &session);
}

static void use_webrtc_opus(void) {
    const uint8_t opus[] = {0xf8u};
    (void)h2_pal_webrtc_peer_send_opus(
        NULL, (h2_pal_webrtc_peer_t *)(uintptr_t)1u, opus, sizeof(opus));
}

static void use_sctp(void) {
    h2_pal_sctp_association_t *association = NULL;
    (void)h2_pal_sctp_association_close(NULL, &association);
}

static void use_json(void) {
    h2_pal_json_document_t *document = NULL;
    h2_pal_json_value_t *value = NULL;
    h2_pal_json_buffer_t buffer = {0};
    (void)h2_pal_json_document_create(NULL, NULL, &document);
    (void)h2_pal_json_document_parse(
        NULL, (const uint8_t *)"null", 4u, NULL, &document);
    (void)h2_pal_json_document_root(NULL, document, &value);
    (void)h2_pal_json_document_serialize(NULL, document, &buffer);
    (void)h2_pal_json_buffer_release(NULL, &buffer);
    (void)h2_pal_json_document_destroy(NULL, &document);
}

static h2_pal_result_t clock_status(void *user, h2_pal_time_wall_status_t *out) {
    int mode = *(int *)user;
    out->valid = mode != 0 && mode != 4;
    return mode == 1 ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static unsigned clock_reads;
static h2_pal_result_t clock_read(void *user, uint64_t *out) {
    ++clock_reads;
    *out = 123;
    return (*(int *)user == 2 || *(int *)user == 4) ? H2_PAL_ERR_UNAVAILABLE : H2_PAL_OK;
}
static void test_valid_wall(void) {
    int mode = 0;
    h2_pal_time_vtable_t vt = {.get_wall_ms = clock_read,
                              .get_wall_status = clock_status};
    h2_pal_time_api_t api = {.user = &mode, .vtable = &vt};
    uint64_t value = 456;
    assert(h2_pal_time_get_wall_ms(&api, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_time_get_wall_ms(NULL, &value) == H2_PAL_ERR_UNSUPPORTED);
    assert(value == 0);
    assert(h2_pal_time_get_wall_ms(&api, &value) == H2_PAL_TIME_ERR_UNCALIBRATED);
    assert(value == 0);
    /* Invalid status wins even if the raw reader would fail. Do not call it. */
    mode = 4;
    value = 456;
    assert(h2_pal_time_get_wall_ms(&api, &value) == H2_PAL_TIME_ERR_UNCALIBRATED);
    assert(value == 0 && clock_reads == 0);
    mode = 1;
    assert(h2_pal_time_get_wall_ms(&api, &value) == H2_PAL_ERR_IO);
    assert(value == 0);
    mode = 2;
    assert(h2_pal_time_get_wall_ms(&api, &value) == H2_PAL_ERR_UNAVAILABLE);
    assert(value == 0);
    mode = 3;
    assert(h2_pal_time_get_wall_ms(&api, &value) == H2_PAL_OK);
    assert(value == 123);
    vt.get_wall_status = NULL;
    assert(h2_pal_time_get_wall_ms(&api, &value) == H2_PAL_ERR_UNSUPPORTED);
    assert(value == 0);
}

static unsigned volume_calls;

static h2_pal_result_t volume_set(void *user, uint32_t percent) {
    (void)user;
    (void)percent;
    volume_calls++;
    return H2_PAL_OK;
}

static h2_pal_result_t volume_get(void *user, uint32_t *out_percent) {
    (void)user;
    volume_calls++;
    *out_percent = 50u;
    return H2_PAL_OK;
}

static void test_call_volume_wrappers(void) {
    uint32_t percent = 0u;
    const h2_pal_modem_vtable_t empty = {0};
    h2_pal_modem_api_t modem = {0};
    assert(H2_PAL_MODEM_CAPABILITY_CALL_VOLUME == (1u << 5));
    assert(h2_pal_modem_set_call_volume(NULL, 50u) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_modem_get_call_volume(NULL, &percent) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_modem_set_call_volume(&modem, 50u) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_modem_get_call_volume(&modem, &percent) == H2_PAL_ERR_UNSUPPORTED);
    modem.vtable = &empty;
    assert(h2_pal_modem_set_call_volume(&modem, 50u) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_modem_get_call_volume(&modem, &percent) == H2_PAL_ERR_UNSUPPORTED);
    const h2_pal_modem_vtable_t implemented = {
        .set_call_volume = volume_set,
        .get_call_volume = volume_get,
    };
    modem.vtable = &implemented;
    assert(h2_pal_modem_set_call_volume(&modem, 101u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_modem_set_call_volume(&modem, UINT32_MAX) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_modem_get_call_volume(&modem, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(volume_calls == 0u);
    assert(h2_pal_modem_set_call_volume(&modem, 50u) == H2_PAL_OK);
    assert(h2_pal_modem_get_call_volume(&modem, &percent) == H2_PAL_OK);
    assert(volume_calls == 2u && percent == 50u);
}

static unsigned legacy_peer_creates;
static h2_pal_result_t legacy_peer_create(void *user,
                                          h2_pal_webrtc_peer_t **out_peer) {
    (void)user;
    ++legacy_peer_creates;
    *out_peer = (h2_pal_webrtc_peer_t *)(uintptr_t)1u;
    return H2_PAL_OK;
}

/* A provider without peer_create_with_config must not silently drop a
 * requested allocator; a default config still reaches peer_create. */
static void test_webrtc_peer_allocator_contract(void) {
    static const h2_pal_webrtc_vtable_t vtable = {
        .peer_create = legacy_peer_create,
    };
    const h2_pal_webrtc_api_t api = {.vtable = &vtable};
    const h2_pal_mem_api_t mem = {0};
    h2_pal_webrtc_peer_t *peer = NULL;
    const h2_pal_webrtc_peer_config_t with_allocator = {.allocator = &mem};
    const h2_pal_webrtc_peer_config_t defaults = {0};
    assert(h2_pal_webrtc_peer_create_with_config(&api, &with_allocator,
                                                 &peer) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(peer == NULL && legacy_peer_creates == 0u);
    assert(h2_pal_webrtc_peer_create_with_config(&api, &defaults, &peer) ==
           H2_PAL_OK);
    assert(peer != NULL && legacy_peer_creates == 1u);
    peer = NULL;
    assert(h2_pal_webrtc_peer_create_with_config(&api, NULL, &peer) ==
           H2_PAL_OK);
    assert(legacy_peer_creates == 2u);
}

int main(void) {
    test_call_volume_wrappers();
    test_valid_wall();
    use_firmware_info();
    use_video_decoder();
    use_wifi_csi();
    use_audio_decoder();
    use_audio_task_names();
    use_serial_host();
    use_crypto();
    use_dtls();
    use_webrtc_opus();
    test_webrtc_peer_allocator_contract();
    use_sctp();
    use_json();
    return 0;
}
