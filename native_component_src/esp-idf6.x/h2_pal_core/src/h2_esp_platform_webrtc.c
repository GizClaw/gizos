#include "h2_esp_platform_core.h"

#include "h2/pal/h2_pal_unsupported.h"
#include "h2_peer.h"
#include "h2_sctp.h"

#include <stdatomic.h>

static atomic_flag h2_esp_h2peer_lock = ATOMIC_FLAG_INIT;
static h2_peer_t *h2_esp_h2peer;
static h2_sctp_t *h2_esp_h2sctp;
static int h2_esp_h2peer_initialized;

static void h2_esp_h2peer_lock_acquire(void) {
    while (atomic_flag_test_and_set_explicit(
        &h2_esp_h2peer_lock, memory_order_acquire)) {
    }
}

static void h2_esp_h2peer_lock_release(void) {
    atomic_flag_clear_explicit(&h2_esp_h2peer_lock, memory_order_release);
}

const h2_pal_webrtc_api_t *h2_esp_platform_webrtc_api(void) {
    h2_esp_h2peer_lock_acquire();
    if (!h2_esp_h2peer_initialized) {
        const h2_sctp_config_t sctp_config = {
            .mem = h2_esp_platform_psram_allocator(),
            .crypto = h2_esp_platform_crypto_api(),
        };
        const h2_peer_config_t config = {
            .mem = h2_esp_platform_psram_allocator(),
            .control_mem = h2_esp_platform_internal_allocator(),
            .log = h2_esp_platform_log_api(),
            .net = h2_esp_platform_net_api(),
            .queue = h2_esp_platform_queue_api(),
            .sync = h2_esp_platform_sync_api(),
            .task = h2_esp_platform_task_api(),
            .time = h2_esp_platform_time_api(),
            .crypto = h2_esp_platform_crypto_api(),
            .dtls = h2_esp_platform_dtls_api(),
            .sctp = NULL,
        };
        h2_esp_h2peer_initialized = 1;
        h2_peer_config_t wired_config = config;
        if (h2_sctp_create(&sctp_config, &h2_esp_h2sctp) == H2_PAL_OK) {
            wired_config.sctp = h2_sctp_api(h2_esp_h2sctp);
        }
        if (wired_config.sctp == NULL ||
            h2_peer_create(&wired_config, &h2_esp_h2peer) != H2_PAL_OK) {
            h2_esp_h2peer = NULL;
            h2_sctp_destroy(&h2_esp_h2sctp);
        }
    }
    const h2_pal_webrtc_api_t *api = h2_esp_h2peer == NULL
        ? h2_pal_unsupported_webrtc_api()
        : h2_peer_webrtc_api(h2_esp_h2peer);
    h2_esp_h2peer_lock_release();
    return api;
}
