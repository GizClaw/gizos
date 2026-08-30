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
    assert(strcmp(h2_pal_audio_mic_task_name,
                  H2_PAL_AUDIO_MIC_TASK_NAME_VALUE) == 0);
    assert(strcmp(h2_pal_audio_mix_task_name,
                  H2_PAL_AUDIO_MIX_TASK_NAME_VALUE) == 0);
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

int main(void) {
    use_firmware_info();
    use_video_decoder();
    use_wifi_csi();
    use_audio_decoder();
    use_audio_task_names();
    use_serial_host();
    use_crypto();
    use_dtls();
    use_webrtc_opus();
    use_sctp();
    use_json();
    return 0;
}
