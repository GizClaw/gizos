#ifndef H2_GIZCLAW_CONFIG_H
#define H2_GIZCLAW_CONFIG_H

#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/application/h2_pal_http.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2/pal/application/h2_pal_webrtc.h"
#include "h2_gizclaw_rpc.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gizclaw_str {
    const char *data;
    size_t len;
} h2_gizclaw_str_t;

typedef enum h2_gizclaw_cipher_mode {
    H2_GIZCLAW_CIPHER_CHACHA20_POLY1305 = 1,
    H2_GIZCLAW_CIPHER_AES_256_GCM = 2,
    H2_GIZCLAW_CIPHER_PLAINTEXT = 3,
} h2_gizclaw_cipher_mode_t;

/** Return true to interrupt the current GizClaw operation. */
typedef bool (*h2_gizclaw_cancel_fn)(void *user);

typedef struct h2_gizclaw_config {
    h2_gizclaw_str_t server_endpoint;
    h2_gizclaw_str_t private_key;
    h2_gizclaw_cipher_mode_t cipher_mode;
    /** @brief Connection timeout in milliseconds; must be positive. */
    int connect_timeout_ms;
    /**
     * @brief Maximum blocking time, in milliseconds, for a WebRTC channel
     * write.
     *
     * Transient PAL backpressure is drained synchronously by waiting for peer
     * progress and retrying the same payload. Providers may wake the wait when
     * their send queue becomes writable. The write stops on success, timeout,
     * cancellation, or a terminal transport error. Cancellation returns
     * `H2_PAL_ERR_CLOSED`. Zero reuses `connect_timeout_ms`; negative values
     * are invalid. A provider that unexpectedly returns
     * `H2_PAL_ERR_WOULD_BLOCK` from a positive-timeout peer poll is rate
     * limited with a logged PAL Time backoff of the lesser of 10 ms and the
     * remaining write timeout. `sleep_ms` remains optional: if this exceptional
     * fallback needs it and the operation is unsupported or fails, the write
     * stops with `H2_PAL_ERR_IO`.
     */
    int write_timeout_ms;
    const h2_pal_mem_api_t *allocator;
    const h2_pal_http_api_t *http;
    const h2_pal_webrtc_api_t *webrtc;
    /**
     * Optional provider-owned bidirectional media track.
     *
     * When set, GizClaw binds it before starting the offer and never accesses
     * codec packets through the PAL. Capture, playback, codec, and RTP
     * progression belong to the WebRTC provider.
     */
    h2_pal_webrtc_track_t *webrtc_media_track;
    const h2_pal_crypto_api_t *crypto;
    const h2_pal_time_api_t *time;
    const h2_pal_log_api_t *log;
    h2_gizclaw_rpc_provider_fn rpc_provider;
    void *rpc_provider_user;
    h2_gizclaw_cancel_fn cancel_requested;
    void *cancel_user;
} h2_gizclaw_config_t;

#ifdef __cplusplus
}
#endif

#endif
