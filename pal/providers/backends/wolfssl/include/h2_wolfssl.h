#ifndef H2_WOLFSSL_H
#define H2_WOLFSSL_H

#include "os/h2_pal_crypto.h"
#include "net/h2_pal_dtls.h"
#include "os/h2_pal_mem.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*h2_wolfssl_entropy_fn)(void* user, uint8_t* out, size_t len);

typedef struct h2_wolfssl_config {
  /** Memory provider copied by value for the complete WolfSSL lifecycle. */
  h2_pal_mem_api_t mem;
  /** Opaque argument passed to entropy. */
  void* entropy_user;
  /** Entropy source required by Crypto and DTLS operations. */
  h2_wolfssl_entropy_fn entropy;
  /**
   * Explicit permission to invoke both Memory and entropy concurrently with
   * each other and with their callers' direct uses. Defaults to false; native
   * asynchronous TLS setup requires true. False preserves inline TLS setup on
   * the calling thread. Its single deadline includes setup and handshake, but
   * an opaque synchronous load cannot be interrupted: timeout is observed when
   * it returns, and no fresh handshake budget is granted. This flag does not
   * extend provider lifetime.
   */
  bool providers_thread_safe;
} h2_wolfssl_config_t;

/**
 * Initializes the process-wide complete WolfSSL provider.
 *
 * Repeated calls with the same Memory/entropy identity and thread-safety flag
 * add owner references. A different configuration is rejected until final
 * cleanup. An exactly matching configuration may resume deferred draining
 * without reinitializing live state. Lifecycle transitions are synchronized;
 * init during initialization or cleanup returns H2_PAL_ERR_INVALID_STATE.
 * The borrowed Memory/entropy providers must remain alive until final cleanup,
 * including after a native TLS timeout. With providers_thread_safe=false,
 * callers must serialize provider callbacks with their own direct uses of the
 * providers.
 */
h2_pal_result_t h2_wolfssl_init(const h2_wolfssl_config_t* config);

/**
 * Deinitializes the provider.
 *
 * This operation is idempotent. Non-final owners release their reference.
 * Releasing the final owner returns H2_PAL_ERR_INVALID_STATE while any DTLS
 * session, native TLS setup, or leased TLS context is alive and leaves that
 * owner reference intact. Retry after those resources finish/release.
 */
h2_pal_result_t h2_wolfssl_deinit(void);

/**
 * Consume this owner's reference without retaining an owner for retry.
 * If this is the final owner, reject new acquisitions until an exactly matching
 * init resumes ownership. Otherwise active operations/contexts retain the
 * backend until their final release, which performs cleanup before returning.
 * With no active operations cleanup happens in this call. No-op when
 * uninitialized or already draining.
 *
 * This API does not own or copy the underlying PAL providers. Their vtables,
 * Memory user context, and entropy user context must outlive deferred cleanup,
 * even after this call returns and the releasing owner is destroyed. Use only
 * when that lifetime is guaranteed independently of the releasing owner. Stop
 * starting new work before releasing an owner; existing leased resources may
 * finish and release. Cleanup may execute on the final releasing operation's
 * thread; provider callback serialization must cover it too when
 * providers_thread_safe=false. Init accepts a new configuration after cleanup.
 */
void h2_wolfssl_deinit_deferred(void);

/**
 * Pin initialized backend state and its borrowed Memory/entropy providers.
 * Acquisition and release may race final deinit: either acquisition succeeds
 * and prevents teardown, or it returns H2_PAL_ERR_INVALID_STATE. Each
 * successful acquire requires exactly one release, after the final WolfSSL
 * call/free. A lease may transfer from a setup worker to the resulting TLS
 * context.
 */
h2_pal_result_t h2_wolfssl_operation_acquire(void);
/**
 * Acquire a lease for work that may call providers concurrently with the
 * caller. Requires providers_thread_safe=true; otherwise returns
 * H2_PAL_ERR_UNSUPPORTED without invoking provider callbacks. Serialized
 * acquisition above remains available without that opt-in. Both reject
 * acquisitions while draining.
 */
h2_pal_result_t h2_wolfssl_operation_acquire_concurrent(void);
void h2_wolfssl_operation_release(void);

/** Returns the ready Crypto PAL provider or the unsupported provider. */
const h2_pal_crypto_api_t* h2_wolfssl_crypto_api(void);

/** Returns the ready DTLS PAL provider or the unsupported provider. */
const h2_pal_dtls_api_t* h2_wolfssl_dtls_api(void);

#ifdef __cplusplus
}
#endif

#endif
