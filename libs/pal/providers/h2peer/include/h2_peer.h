#ifndef H2_PEER_H
#define H2_PEER_H

#include "h2/pal/application/h2_pal_webrtc.h"
#include "h2/pal/net/h2_pal_dtls.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/net/h2_pal_sctp.h"
#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque owner of H2Peer connections and their borrowed WebRTC API view. */
typedef struct h2_peer h2_peer_t;

/**
 * Platform capabilities borrowed by one H2Peer instance.
 *
 * Every API object and its backend state must remain valid until
 * h2_peer_destroy() clears the owner pointer. The Memory and control Memory
 * APIs must additionally outlive every owned event, including events released
 * after destruction. H2Peer never closes or frees these providers.
 */
typedef struct h2_peer_config {
  /** Required borrowed allocator used for every package-owned allocation. */
  const h2_pal_mem_api_t *mem;
  /**
   * Optional borrowed allocator for state containing cross-task atomics.
   * When NULL, `mem` is used. Platforms whose atomic instructions cannot
   * target external memory must provide an internal-memory allocator here.
   */
  const h2_pal_mem_api_t *control_mem;
  /**
   * Required borrowed logger used for all bounded H2Peer diagnostics. Its
   * API object and backend state must remain valid for the owner lifetime.
   */
  const h2_pal_log_api_t *log;
  /**
   * Required borrowed socket capability used by ICE transport providers.
   * Production H2Peer requires UDP open/send/receive plus bounded active TCP
   * client open/open-bound/connect/send-timeout/receive/close operations.
   * TCP is used only for RFC 6544 active-to-passive ICE candidates; TURN over
   * TCP/TLS and local passive listening are not supported.
   */
  const h2_pal_net_api_t *net;
  /** Required borrowed bounded queue capability used by peer worker tasks. */
  const h2_pal_queue_api_t *queue;
  /** Required borrowed synchronization capability used by protocol state. */
  const h2_pal_sync_api_t *sync;
  /** Required borrowed task capability used by peer worker tasks. */
  const h2_pal_task_api_t *task;
  /**
   * Required borrowed millisecond and microsecond monotonic clocks used by
   * bounded polling, timeouts, and protocol-owner performance telemetry.
   */
  const h2_pal_time_api_t *time;
  /** Required borrowed entropy and cryptographic capability. */
  const h2_pal_crypto_api_t *crypto;
  /** Required borrowed datagram TLS capability used by WebRTC transports. */
  const h2_pal_dtls_api_t *dtls;
  /** Required borrowed SCTP capability used by WebRTC DataChannels. */
  const h2_pal_sctp_api_t *sctp;
} h2_peer_config_t;

/**
 * Creates an H2Peer instance backed by caller-owned PAL capabilities.
 *
 * The package initializes its private ICE and SRTP orchestration from portable
 * dependencies. DTLS and SCTP providers are injected by the composition owner.
 * libSRTP retains process-global state, so concurrent instances must borrow
 * Memory and Crypto APIs with the same backend identity (equal user and vtable
 * fields). Provider initialization fails explicitly and never falls back to a
 * fake or plaintext transport.
 *
 * @param config Required borrowed PAL capabilities.
 * @param out_peer Receives the owned instance and is cleared on failure.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG, H2_PAL_ERR_UNSUPPORTED, or
 *     H2_PAL_ERR_NO_MEMORY.
 */
h2_pal_result_t h2_peer_create(const h2_peer_config_t *config,
                               h2_peer_t **out_peer);

/**
 * Returns the WebRTC PAL view borrowed from an H2Peer instance.
 *
 * The returned pointer remains valid until h2_peer_destroy(). It must not be
 * modified or retained beyond the instance lifetime.
 *
 * Each production WebRTC peer owns one protocol task. Direct selected UDP
 * transports also use a reader task that only receives datagrams; all protocol
 * state remains owned by the protocol task. Public control calls synchronously
 * marshal work to the protocol task. DataChannel and Opus send calls copy input
 * into bounded peer-owned mailboxes and can return H2_PAL_ERR_WOULD_BLOCK
 * without consuming input when no slot is available. Network events are
 * returned one at a time by h2_pal_webrtc_peer_poll(); H2Peer never invokes
 * user callbacks from its protocol task.
 *
 * Serialize peer creation, peer close and owner destruction on the lifecycle
 * caller. Stop other calls using a handle before closing it. Distinct owned
 * events may be released concurrently, including during or after destruction.
 *
 * @param peer Required live H2Peer instance.
 * @return Borrowed WebRTC API view, or NULL for an invalid instance.
 */
const h2_pal_webrtc_api_t *h2_peer_webrtc_api(h2_peer_t *peer);

/**
 * Closes all live peers and channels and destroys an H2Peer instance.
 *
 * Passing NULL or a pointer containing NULL is a successful no-op. After all
 * peer tasks have joined, the caller pointer is cleared. Owned events remain
 * valid until released. Injected PAL capabilities are never destroyed.
 *
 * If a PAL close command or task join fails, the pointer is retained for a
 * later destroy retry; no new peers may be created on that owner. Keep its
 * borrowed capabilities alive while the pointer remains non-NULL.
 *
 * @param peer Address of the owned instance pointer.
 */
void h2_peer_destroy(h2_peer_t **peer);

#ifdef __cplusplus
}
#endif

#endif
