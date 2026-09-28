#include "h2/pal/h2_pal_unsupported.h"
#include "h2_android_platform.h"
#include "h2_android_tls_internal.h"
#include "h2_peer.h"
#include "h2_posix_pal_core.h"
#include "h2_sctp.h"
#include "h2_wolfssl.h"
#include <string.h>
struct h2_android_webrtc {
  h2_peer_t *peer;
  h2_sctp_t *sctp;
};
h2_pal_result_t h2_android_webrtc_create(h2_android_webrtc_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  int rc = h2_android_tls_acquire();
  if (rc != H2_PAL_OK)
    return (h2_pal_result_t)rc;
  const h2_pal_mem_api_t *memory = h2_android_platform_mem_api();
  h2_android_webrtc_t *owner = h2_pal_mem_alloc(memory, sizeof(*owner));
  if (!owner) {
    (void)h2_android_tls_release();
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(owner, 0, sizeof(*owner));
  const h2_sctp_config_t sctp = {.mem = memory,
                                 .crypto = h2_wolfssl_crypto_api()};
  rc = h2_sctp_create(&sctp, &owner->sctp);
  if (rc == H2_PAL_OK) {
    const h2_peer_config_t config = {
        .mem = memory,
        .log = h2_android_platform_log_api(),
        .net = h2_posix_net_api(),
        .queue = h2_android_platform_queue_api(),
        .sync = h2_android_platform_sync_api(),
        .task = h2_android_platform_task_api(),
        .time = h2_android_platform_time_api(),
        .crypto = h2_wolfssl_crypto_api(),
        .dtls = h2_wolfssl_dtls_api(),
        .sctp = h2_sctp_api(owner->sctp),
    };
    rc = h2_peer_create(&config, &owner->peer);
  }
  if (rc != H2_PAL_OK) {
    (void)h2_sctp_destroy(&owner->sctp);
    h2_pal_mem_free(memory, owner);
    (void)h2_android_tls_release();
    return (h2_pal_result_t)rc;
  }
  *out = owner;
  return H2_PAL_OK;
}
const h2_pal_webrtc_api_t *h2_android_webrtc_api(h2_android_webrtc_t *owner) {
  return owner ? h2_peer_webrtc_api(owner->peer)
               : h2_pal_unsupported_webrtc_api();
}
h2_pal_result_t h2_android_webrtc_destroy(h2_android_webrtc_t **owner) {
  if (!owner || !*owner)
    return H2_PAL_OK;
  h2_peer_destroy(&(*owner)->peer);
  if ((*owner)->peer)
    return H2_PAL_ERR_BUSY;
  int rc = h2_sctp_destroy(&(*owner)->sctp);
  if (rc != H2_PAL_OK)
    return (h2_pal_result_t)rc;
  rc = h2_android_tls_release();
  if (rc != H2_PAL_OK)
    return (h2_pal_result_t)rc;
  h2_pal_mem_free(h2_android_platform_mem_api(), *owner);
  *owner = NULL;
  return H2_PAL_OK;
}
