#include "h2/pal/hal/h2_pal_camera.h"
#include "h2_pal.h"

#include <cassert>
#include <cstring>

static void use_firmware_info() {
  h2_pal_firmware_info_t info{};
  (void)h2_pal_firmware_info_get_current(nullptr, &info);
  h2_pal_device_identity_t identity{};
  h2_pal_mem_stats_t stats{};
  (void)h2_pal_device_identity_get(nullptr, &identity);
  (void)h2_pal_mem_get_stats(nullptr, H2_PAL_MEM_REGION_INTERNAL, &stats);
  h2_pal_image_info_t image{};
  h2_pal_image_write_request_t write{1u, 1u, 1u};
  h2_pal_power_boot_request_t boot{1u, H2_PAL_POWER_BOOT_POLICY_TRIAL};
  h2_pal_power_native_evidence_t evidence{};
  h2_pal_power_image_confirmation_t confirmation{};
  (void)h2_pal_image_get_info(nullptr, 1u, H2_PAL_IMAGE_VIEW_STORED, &image);
  (void)h2_pal_image_begin(nullptr, &write);
  (void)h2_pal_power_prepare_next_boot_partition(nullptr, &boot, &evidence);
  (void)h2_pal_power_confirm_running_image(nullptr, &evidence, &confirmation);
}

static void use_video_decoder() {
  h2_pal_video_decoder_session_t* session = nullptr;
  const h2_video_decoder_config_t config{};
  (void)h2_pal_video_decoder_open(nullptr, &config, &session);
}

static void use_audio_decoder() {
  h2_pal_audio_decoder_session_t* session = nullptr;
  const h2_audio_decoder_config_t config{};
  (void)h2_pal_audio_decoder_open(nullptr, &config, &session);
}

static void use_audio_task_names() {
  assert(std::strcmp(H2_PAL_AUDIO_MIC_TASK_NAME_VALUE, "$audio/mic") == 0);
  assert(std::strcmp(H2_PAL_AUDIO_MIX_TASK_NAME_VALUE, "$audio/mix") == 0);
}

static void use_wifi_csi() {
  h2_pal_wifi_csi_capabilities_t capabilities{};
  (void)h2_pal_wifi_csi_get_capabilities(nullptr, &capabilities);
}

static void use_io_stream() {
  char byte = 0;
  size_t count = 99u;
  assert(h2_pal_io_stream_start(nullptr) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_io_stream_read(nullptr, &byte, 1u, &count, 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(count == 0u);
  assert(h2_pal_io_stream_write(nullptr, &byte, 1u, &count, 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(count == 0u);
  assert(h2_pal_io_stream_flush(nullptr) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_io_stream_release(nullptr) == H2_PAL_ERR_INVALID_ARG);
}

static void use_serial_host() {
  h2_pal_serial_host_snapshot_t* snapshot = nullptr;
  (void)h2_pal_serial_host_scan(nullptr, &snapshot);
  (void)h2_pal_serial_host_snapshot_destroy(nullptr, &snapshot);
}

static void use_crypto() {
  h2_pal_p256_private_key_t private_key{};
  h2_pal_p256_public_key_t public_key{};
  h2_pal_p256_keypair_t keypair{};
  h2_pal_p256_signature_t signature{};
  (void)h2_pal_crypto_p256_keypair_from_private(nullptr, &private_key,
                                                &keypair);
  (void)h2_pal_crypto_p256_keypair_generate(nullptr, &keypair);
  (void)h2_pal_crypto_p256_public_key_validate(nullptr, &public_key);
  (void)h2_pal_crypto_ecdsa_p256_sha256_sign(nullptr, &private_key, nullptr, 0u,
                                             &signature);
  (void)h2_pal_crypto_ecdsa_p256_sha256_verify(nullptr, &public_key, nullptr,
                                               0u, &signature);
}

static void use_dtls() {
  h2_pal_dtls_session_t* session = nullptr;
  h2_pal_dtls_session_destroy(nullptr, &session);
}

static void use_webrtc_opus() {
  const uint8_t opus[]{0xf8u};
  (void)h2_pal_webrtc_peer_send_opus(
      nullptr, reinterpret_cast<h2_pal_webrtc_peer_t*>(1u), opus, sizeof(opus));
}

static void use_sctp() {
  h2_pal_sctp_association_t* association = nullptr;
  (void)h2_pal_sctp_association_close(nullptr, &association);
}

static void use_json() {
  h2_pal_json_document_t* document = nullptr;
  h2_pal_json_value_t* value = nullptr;
  h2_pal_json_buffer_t buffer{};
  (void)h2_pal_json_document_create(nullptr, nullptr, &document);
  (void)h2_pal_json_document_parse(nullptr,
                                   reinterpret_cast<const uint8_t*>("null"), 4u,
                                   nullptr, &document);
  (void)h2_pal_json_document_root(nullptr, document, &value);
  (void)h2_pal_json_document_serialize(nullptr, document, &buffer);
  (void)h2_pal_json_buffer_release(nullptr, &buffer);
  (void)h2_pal_json_document_destroy(nullptr, &document);
}

int main() {
  use_firmware_info();
  use_video_decoder();
  use_wifi_csi();
  use_audio_decoder();
  use_audio_task_names();
  use_io_stream();
  use_serial_host();
  use_crypto();
  use_dtls();
  use_webrtc_opus();
  use_sctp();
  use_json();
  return 0;
}
