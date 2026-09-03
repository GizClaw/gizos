#include "h2_pal.h"

#include <assert.h>
#include <string.h>

static uint8_t s_allocator_storage[64];

static void *fixture_alloc(void *user, size_t size) {
    (void)user;
    return size <= sizeof(s_allocator_storage) ? s_allocator_storage : NULL;
}

static void fixture_free(void *user, void *pointer) {
    (void)user;
    (void)pointer;
}

static const h2_pal_mem_vtable_t s_allocator_vtable = {
    .alloc = fixture_alloc,
    .realloc = NULL,
    .free = fixture_free,
};

static const h2_pal_mem_api_t s_allocator = {
    .user = NULL,
    .vtable = &s_allocator_vtable,
};

#define CHECK_UNSUPPORTED_API(name)                         \
    do {                                                    \
        assert(h2_pal_unsupported_##name##_api() != NULL); \
        assert(h2_pal_unsupported_##name##_api()->vtable != NULL); \
    } while (0)

typedef enum firmware_info_behavior {
    FIRMWARE_INFO_VALID,
    FIRMWARE_INFO_EMPTY,
    FIRMWARE_INFO_UNTERMINATED,
    FIRMWARE_INFO_PARTIAL_FAILURE,
} firmware_info_behavior_t;

typedef struct firmware_info_fixture {
    firmware_info_behavior_t behavior;
    char version[H2_PAL_FIRMWARE_VERSION_MAX];
} firmware_info_fixture_t;

static h2_pal_result_t fixture_get_current(
    void *user,
    h2_pal_firmware_info_t *out_info) {
    firmware_info_fixture_t *fixture = (firmware_info_fixture_t *)user;

    if (fixture->behavior == FIRMWARE_INFO_EMPTY) {
        out_info->version[0] = '\0';
        return H2_PAL_OK;
    }
    if (fixture->behavior == FIRMWARE_INFO_UNTERMINATED) {
        memset(out_info->version, 'x', sizeof(out_info->version));
        return H2_PAL_OK;
    }
    if (fixture->behavior == FIRMWARE_INFO_PARTIAL_FAILURE) {
        memcpy(out_info->version, "partial", sizeof("partial"));
        return H2_PAL_ERR_IO;
    }
    memcpy(out_info->version, fixture->version, sizeof(out_info->version));
    return H2_PAL_OK;
}

static const h2_pal_firmware_info_vtable_t firmware_info_fixture_vtable = {
    .get_current = fixture_get_current,
};

static void discard_wifi_csi_frame(
    void *user,
    const h2_pal_wifi_csi_frame_t *frame) {
    (void)user;
    (void)frame;
}

static void test_firmware_info_contract(void) {
    firmware_info_fixture_t fixture = {
        .behavior = FIRMWARE_INFO_VALID,
        .version = "release-123",
    };
    const h2_pal_firmware_info_api_t api = {
        .user = &fixture,
        .vtable = &firmware_info_fixture_vtable,
    };
    h2_pal_firmware_info_t firmware_info;

    assert(h2_pal_firmware_info_get_current(&api, &firmware_info) == H2_PAL_OK);
    assert(strcmp(firmware_info.version, "release-123") == 0);
    fixture.version[0] = 'X';
    assert(strcmp(firmware_info.version, "release-123") == 0);

    fixture.behavior = FIRMWARE_INFO_EMPTY;
    memset(&firmware_info, 0x5a, sizeof(firmware_info));
    assert(h2_pal_firmware_info_get_current(&api, &firmware_info) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(firmware_info.version[0] == '\0');

    fixture.behavior = FIRMWARE_INFO_UNTERMINATED;
    assert(h2_pal_firmware_info_get_current(&api, &firmware_info) ==
           H2_PAL_ERR_TRUNCATED);
    assert(firmware_info.version[0] == '\0');

    fixture.behavior = FIRMWARE_INFO_PARTIAL_FAILURE;
    assert(h2_pal_firmware_info_get_current(&api, &firmware_info) ==
           H2_PAL_ERR_IO);
    assert(firmware_info.version[0] == '\0');
}

int main(void) {
    static const h2_pal_firmware_info_vtable_t partial_vtable = {0};
    const h2_pal_firmware_info_api_t missing_vtable_api = {
        .user = NULL,
        .vtable = NULL,
    };
    const h2_pal_firmware_info_api_t partial_api = {
        .user = NULL,
        .vtable = &partial_vtable,
    };
    h2_pal_firmware_info_t firmware_info;
    memset(&firmware_info, 0x5a, sizeof(firmware_info));
    assert(h2_pal_firmware_info_get_current(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_firmware_info_get_current(NULL, &firmware_info) == H2_PAL_ERR_UNSUPPORTED);
    assert(firmware_info.version[0] == '\0');
    memset(&firmware_info, 0x5a, sizeof(firmware_info));
    assert(h2_pal_firmware_info_get_current(
               &missing_vtable_api,
               &firmware_info) == H2_PAL_ERR_INVALID_ARG);
    assert(firmware_info.version[0] == '\0');
    memset(&firmware_info, 0x5a, sizeof(firmware_info));
    assert(h2_pal_firmware_info_get_current(
               &partial_api,
               &firmware_info) == H2_PAL_ERR_INVALID_ARG);
    assert(firmware_info.version[0] == '\0');
    test_firmware_info_contract();
    memset(&firmware_info, 0x5a, sizeof(firmware_info));
    assert(h2_pal_firmware_info_get_current(
               h2_pal_unsupported_firmware_info_api(),
               &firmware_info) == H2_PAL_ERR_UNSUPPORTED);
    assert(firmware_info.version[0] == '\0');
    CHECK_UNSUPPORTED_API(audio);
    CHECK_UNSUPPORTED_API(audio_decoder);
    CHECK_UNSUPPORTED_API(ble_host);
    CHECK_UNSUPPORTED_API(button);
    CHECK_UNSUPPORTED_API(touch);
    CHECK_UNSUPPORTED_API(buzzer);
    CHECK_UNSUPPORTED_API(crypto);
    assert(H2_PAL_CRYPTO_AEAD_AES_128_GCM == 1);
    assert(H2_PAL_CRYPTO_AEAD_AES_256_GCM == 2);
    assert(H2_PAL_CRYPTO_AEAD_CHACHA20_POLY1305 == 3);
    const h2_pal_crypto_api_t *crypto = h2_pal_unsupported_crypto_api();
    assert(crypto->vtable->x25519_keypair_generate != NULL);
    assert(crypto->vtable->x25519_public_key_from_private != NULL);
    assert(crypto->vtable->x25519_shared_secret != NULL);
    assert(crypto->vtable->aes_ctr_xor != NULL);
    assert(crypto->vtable->md5 != NULL);
    assert(crypto->vtable->hmac_sha1 != NULL);
    assert(crypto->vtable->p256_keypair_from_private != NULL);
    assert(crypto->vtable->p256_keypair_generate != NULL);
    assert(crypto->vtable->p256_public_key_validate != NULL);
    assert(crypto->vtable->ecdsa_p256_sha256_sign != NULL);
    assert(crypto->vtable->ecdsa_p256_sha256_verify != NULL);
    h2_pal_p256_private_key_t p256_private = {0};
    h2_pal_p256_public_key_t p256_public = {0};
    h2_pal_p256_keypair_t p256_keypair = {0};
    h2_pal_p256_signature_t p256_signature = {0};
    memset(&p256_keypair, 0xa5, sizeof(p256_keypair));
    assert(h2_pal_crypto_p256_keypair_from_private(
               crypto, &p256_private, &p256_keypair) == H2_PAL_ERR_UNSUPPORTED);
    assert(p256_keypair.private_key.bytes[0] == 0u);
    memset(&p256_keypair, 0xa5, sizeof(p256_keypair));
    assert(h2_pal_crypto_p256_keypair_generate(
               crypto, &p256_keypair) == H2_PAL_ERR_UNSUPPORTED);
    assert(p256_keypair.private_key.bytes[0] == 0u);
    assert(h2_pal_crypto_p256_public_key_validate(
               crypto, &p256_public) == H2_PAL_ERR_UNSUPPORTED);
    memset(&p256_signature, 0xa5, sizeof(p256_signature));
    assert(h2_pal_crypto_ecdsa_p256_sha256_sign(
               crypto, &p256_private, NULL, 0u,
               &p256_signature) == H2_PAL_ERR_UNSUPPORTED);
    assert(p256_signature.bytes[0] == 0u);
    assert(h2_pal_crypto_ecdsa_p256_sha256_verify(
               crypto, &p256_public, NULL, 0u,
               &p256_signature) == H2_PAL_ERR_UNSUPPORTED);
    CHECK_UNSUPPORTED_API(disk);
    CHECK_UNSUPPORTED_API(dtls);
    CHECK_UNSUPPORTED_API(display);
    CHECK_UNSUPPORTED_API(fs);
    CHECK_UNSUPPORTED_API(gpio_irq);
    CHECK_UNSUPPORTED_API(http);
    CHECK_UNSUPPORTED_API(imu);
    CHECK_UNSUPPORTED_API(input);
    CHECK_UNSUPPORTED_API(json);
    CHECK_UNSUPPORTED_API(led);
    CHECK_UNSUPPORTED_API(log);
    CHECK_UNSUPPORTED_API(mem);
    CHECK_UNSUPPORTED_API(modem);
    CHECK_UNSUPPORTED_API(mqtt);
    CHECK_UNSUPPORTED_API(net);
    CHECK_UNSUPPORTED_API(netif);
    CHECK_UNSUPPORTED_API(nfc);
    CHECK_UNSUPPORTED_API(periph);
    CHECK_UNSUPPORTED_API(power);
    CHECK_UNSUPPORTED_API(pref);
    CHECK_UNSUPPORTED_API(pwm_switch);
    CHECK_UNSUPPORTED_API(queue);
    CHECK_UNSUPPORTED_API(sctp);
    CHECK_UNSUPPORTED_API(serial_host);
    CHECK_UNSUPPORTED_API(switch);
    CHECK_UNSUPPORTED_API(sync);
    CHECK_UNSUPPORTED_API(system_event);
    CHECK_UNSUPPORTED_API(task);
    CHECK_UNSUPPORTED_API(time);
    CHECK_UNSUPPORTED_API(timer);
    CHECK_UNSUPPORTED_API(uart_io_stream);
    CHECK_UNSUPPORTED_API(usb_jtag_io_stream);
    CHECK_UNSUPPORTED_API(video_decoder);
    CHECK_UNSUPPORTED_API(webrtc);
    CHECK_UNSUPPORTED_API(wifi_ap);
    CHECK_UNSUPPORTED_API(wifi_csi);
    CHECK_UNSUPPORTED_API(wifi_settings);
    CHECK_UNSUPPORTED_API(wifi_sta);

    h2_pal_json_document_t *json_document =
        (h2_pal_json_document_t *)(uintptr_t)1u;
    assert(h2_pal_json_document_parse(
               h2_pal_unsupported_json_api(),
               (const uint8_t *)"null", 4u, NULL,
               &json_document) == H2_PAL_ERR_UNSUPPORTED);
    assert(json_document == NULL);
    assert(h2_pal_json_document_create(
               h2_pal_unsupported_json_api(), NULL,
               &json_document) == H2_PAL_ERR_UNSUPPORTED);
    assert(json_document == NULL);

    h2_pal_serial_host_snapshot_t *serial_snapshot =
        (h2_pal_serial_host_snapshot_t *)(uintptr_t)1u;
    assert(h2_pal_serial_host_scan(
               h2_pal_unsupported_serial_host_api(),
               &serial_snapshot) == H2_PAL_ERR_UNSUPPORTED);
    assert(serial_snapshot == NULL);
    const h2_pal_uart_io_stream_config_t serial_config = {
        .baud_rate = 115200u,
        .data_bits = 8u,
        .stop_bits = 1u,
        .parity = H2_PAL_UART_PARITY_NONE,
        .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
    };
    h2_pal_serial_host_session_t *serial_session =
        (h2_pal_serial_host_session_t *)(uintptr_t)1u;
    assert(h2_pal_serial_host_open(
               h2_pal_unsupported_serial_host_api(),
               "",
               &serial_config,
               &serial_session) == H2_PAL_ERR_INVALID_ARG);
    assert(serial_session == NULL);
    assert(h2_pal_serial_host_open(
               h2_pal_unsupported_serial_host_api(),
               "unsupported",
               &serial_config,
               &serial_session) == H2_PAL_ERR_UNSUPPORTED);
    assert(serial_session == NULL);

    assert(h2_pal_ble_start(h2_pal_unsupported_ble_host_api()) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_wifi_sta_disconnect(h2_pal_unsupported_wifi_sta_api()) == H2_PAL_ERR_UNSUPPORTED);
    h2_pal_wifi_csi_capabilities_t csi_capabilities = {0};
    h2_pal_wifi_csi_config_t csi_config = {0};
    assert(h2_pal_wifi_csi_get_capabilities(
               h2_pal_unsupported_wifi_csi_api(),
               &csi_capabilities) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_wifi_csi_start(
               h2_pal_unsupported_wifi_csi_api(),
               &csi_config,
               discard_wifi_csi_frame,
               NULL) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_wifi_csi_stop(h2_pal_unsupported_wifi_csi_api()) == H2_PAL_OK);
    assert(h2_pal_power_reboot(h2_pal_unsupported_power_api(), 0u) == H2_PAL_ERR_UNSUPPORTED);
    const h2_pal_net_api_t *unsupported_net = h2_pal_unsupported_net_api();
    assert(unsupported_net->vtable->tcp_send_timeout != NULL);
    const uint8_t tcp_byte = 0u;
    assert(h2_pal_net_tcp_send_timeout(
               unsupported_net, 1, &tcp_byte, sizeof(tcp_byte), 1u) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(unsupported_net->vtable->tcp_listen != NULL);
    assert(unsupported_net->vtable->tcp_accept != NULL);
    h2_pal_net_socket_t listen_socket = 7;
    h2_pal_net_addr_t listen_addr;
    assert(h2_pal_net_tcp_listen(
               unsupported_net, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
               &listen_socket, &listen_addr) == H2_PAL_ERR_UNSUPPORTED);
    assert(listen_socket == -1);
    h2_pal_net_socket_t accepted_socket = 7;
    assert(h2_pal_net_tcp_accept(
               unsupported_net, 1, &accepted_socket, NULL, 1u) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(accepted_socket == -1);
    const h2_pal_webrtc_ice_server_t ice_server = {
        .url = {.data = "stun:example.invalid", .len = 20u},
    };
    h2_pal_webrtc_peer_t *webrtc_peer = NULL;
    assert(h2_pal_webrtc_peer_create(
               h2_pal_unsupported_webrtc_api(),
               &webrtc_peer) == H2_PAL_ERR_UNSUPPORTED);
    h2_pal_webrtc_track_t webrtc_track = {
        .native_handle = (void *)(uintptr_t)1u,
    };
    assert(h2_pal_webrtc_peer_set_track(
               h2_pal_unsupported_webrtc_api(),
               (h2_pal_webrtc_peer_t *)(uintptr_t)1u,
               &webrtc_track) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_webrtc_peer_unset_track(
               h2_pal_unsupported_webrtc_api(),
               (h2_pal_webrtc_peer_t *)(uintptr_t)1u,
               &webrtc_track) == H2_PAL_ERR_UNSUPPORTED);
    h2_pal_webrtc_event_t webrtc_event = {0};
    assert(h2_pal_webrtc_peer_poll(
               h2_pal_unsupported_webrtc_api(),
               (h2_pal_webrtc_peer_t *)(uintptr_t)1u, 0,
               &webrtc_event) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_webrtc_peer_add_ice_server(
               h2_pal_unsupported_webrtc_api(),
               (h2_pal_webrtc_peer_t *)(uintptr_t)1u,
               &ice_server) == H2_PAL_ERR_UNSUPPORTED);
    const uint8_t opus[] = {0xf8u};
    assert(h2_pal_webrtc_peer_send_opus(
               h2_pal_unsupported_webrtc_api(),
               (h2_pal_webrtc_peer_t *)(uintptr_t)1u,
               opus,
               sizeof(opus)) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_webrtc_peer_send_opus(
               h2_pal_unsupported_webrtc_api(),
               (h2_pal_webrtc_peer_t *)(uintptr_t)1u,
               NULL,
               0u) == H2_PAL_ERR_INVALID_ARG);
    const h2_video_decoder_config_t config = {
        .frame_allocator = &s_allocator,
    };
    h2_pal_video_decoder_session_t *session = (void *)(uintptr_t)1u;
    assert(h2_pal_video_decoder_open(
               h2_pal_unsupported_video_decoder_api(),
               &config,
               &session) == H2_PAL_ERR_UNSUPPORTED);
    assert(session == NULL);

    const h2_audio_decoder_config_t audio_config = {
        .pcm_allocator = &s_allocator,
        .preferred_format = H2_AUDIO_SAMPLE_S16LE,
    };
    h2_pal_audio_decoder_session_t *audio_session = (void *)(uintptr_t)1u;
    assert(h2_pal_audio_decoder_open(
               h2_pal_unsupported_audio_decoder_api(),
               &audio_config,
               &audio_session) == H2_PAL_ERR_UNSUPPORTED);
    assert(audio_session == NULL);
    return 0;
}

#undef CHECK_UNSUPPORTED_API
