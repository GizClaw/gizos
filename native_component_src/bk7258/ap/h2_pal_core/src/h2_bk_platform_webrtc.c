#include "h2_bk_platform_core.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_peer.h"
#include "h2_sctp.h"

#include "h2_atomic.h"

static h2_peer_t *h2_bk_platform_peer;
static h2_sctp_t *h2_bk_platform_sctp;
static h2_atomic_flag_t h2_bk_platform_peer_lock = {0};
static int h2_bk_platform_peer_initialized;
static bool h2_bk_platform_peer_lock_ready;

h2_pal_result_t h2_bk_platform_webrtc_atomic_init(void) {
    if (h2_bk_platform_peer_lock_ready) return H2_PAL_ERR_INVALID_STATE;
    h2_pal_result_t rc = h2_peer_global_init();
    if (rc != H2_PAL_OK) return rc;
    const h2_atomic_result_t atomic_rc = h2_atomic_flag_init(&h2_bk_platform_peer_lock);
    if (atomic_rc != H2_ATOMIC_OK) {
        (void)h2_peer_global_shutdown();
        return atomic_rc == H2_ATOMIC_UNSUPPORTED
            ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_ERR_NO_MEMORY;
    }
    h2_bk_platform_peer_lock_ready = true;
    return H2_PAL_OK;
}

h2_pal_result_t h2_bk_platform_webrtc_atomic_shutdown(void) {
    if (!h2_bk_platform_peer_lock_ready) return H2_PAL_ERR_INVALID_STATE;
    if (h2_atomic_flag_test_and_set(&h2_bk_platform_peer_lock,
                                    H2_ATOMIC_ACQUIRE)) return H2_PAL_ERR_BUSY;
    h2_peer_destroy(&h2_bk_platform_peer);
    if (h2_bk_platform_peer != NULL) {
        h2_atomic_flag_clear(&h2_bk_platform_peer_lock, H2_ATOMIC_RELEASE);
        return H2_PAL_ERR_BUSY;
    }
    h2_sctp_destroy(&h2_bk_platform_sctp);
    h2_pal_result_t rc = h2_peer_global_shutdown();
    if (rc != H2_PAL_OK) {
        h2_atomic_flag_clear(&h2_bk_platform_peer_lock, H2_ATOMIC_RELEASE);
        return rc;
    }
    h2_bk_platform_peer_initialized = 0;
    h2_bk_platform_peer_lock_ready = false;
    h2_atomic_flag_destroy(&h2_bk_platform_peer_lock);
    return H2_PAL_OK;
}

static void h2_bk_platform_peer_lock_acquire(void) {
    while (h2_atomic_flag_test_and_set(
        &h2_bk_platform_peer_lock, H2_ATOMIC_ACQUIRE)) {
    }
}

static void h2_bk_platform_peer_lock_release(void) {
    h2_atomic_flag_clear(
        &h2_bk_platform_peer_lock, H2_ATOMIC_RELEASE);
}

const h2_pal_webrtc_api_t *h2_bk_platform_webrtc_api(void) {
    if (!h2_bk_platform_peer_lock_ready) return h2_pal_unsupported_webrtc_api();
    h2_bk_platform_peer_lock_acquire();
    if (!h2_bk_platform_peer_initialized) {
        const h2_sctp_config_t sctp_config = {
            .mem = h2_bk_platform_psram_allocator(),
            .crypto = h2_bk_platform_crypto_api(),
        };
        const h2_peer_config_t config = {
            .mem = h2_bk_platform_psram_allocator(),
            .log = h2_bk_platform_log_api(),
            .net = h2_bk_platform_net_api(),
            .queue = h2_bk_platform_queue_api(),
            .sync = h2_bk_platform_sync_api(),
            .task = h2_bk_platform_task_api(),
            .time = h2_bk_platform_time_api(),
            .crypto = h2_bk_platform_crypto_api(),
            .dtls = h2_bk_platform_dtls_api(),
            .sctp = NULL,
        };
        h2_bk_platform_peer_initialized = 1;
        h2_peer_config_t wired_config = config;
        if (h2_sctp_create(&sctp_config, &h2_bk_platform_sctp) == H2_PAL_OK) {
            wired_config.sctp = h2_sctp_api(h2_bk_platform_sctp);
        }
        if (wired_config.sctp == NULL ||
            h2_peer_create(&wired_config, &h2_bk_platform_peer) != H2_PAL_OK) {
            h2_bk_platform_peer = NULL;
            h2_sctp_destroy(&h2_bk_platform_sctp);
        }
    }
    const h2_pal_webrtc_api_t *api = h2_bk_platform_peer == NULL
        ? h2_pal_unsupported_webrtc_api()
        : h2_peer_webrtc_api(h2_bk_platform_peer);
    h2_bk_platform_peer_lock_release();
    return api;
}
