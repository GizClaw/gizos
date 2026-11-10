#include "h2_wolfssl_internal.h"

#include "h2_pal_unsupported.h"
#include "h2_wolfcrypt_crypto.h"

#include <stdatomic.h>
#include <string.h>

#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/memory.h>

typedef struct h2_wolfssl_state {
  void* entropy_user;
  h2_wolfssl_entropy_fn entropy;
  wolfSSL_Malloc_cb previous_malloc;
  wolfSSL_Free_cb previous_free;
  wolfSSL_Realloc_cb previous_realloc;
  size_t owner_refs;
  int ready;
} h2_wolfssl_state_t;

h2_pal_mem_api_t h2_wolfssl_mem_api;
static h2_wolfssl_state_t h2_wolfssl_state;
/* Admission, draining, and the active count change atomically. A draining
 * backend has no owner references; matching init may pin and resume it. */
#define H2_WOLFSSL_READY (SIZE_MAX / 2u + 1u)
#define H2_WOLFSSL_DRAINING (H2_WOLFSSL_READY >> 1u)
#define H2_WOLFSSL_PHASE (H2_WOLFSSL_READY | H2_WOLFSSL_DRAINING)
#define H2_WOLFSSL_BUSY H2_WOLFSSL_PHASE
#define H2_WOLFSSL_CONCURRENT (H2_WOLFSSL_DRAINING >> 1u)
#define H2_WOLFSSL_COUNT (H2_WOLFSSL_CONCURRENT - 1u)
static atomic_size_t h2_wolfssl_usage;
static atomic_flag h2_wolfssl_lifecycle = ATOMIC_FLAG_INIT;

static void lifecycle_lock(void) {
  while (atomic_flag_test_and_set_explicit(&h2_wolfssl_lifecycle,
                                           memory_order_acquire)) {
  }
}

static void lifecycle_unlock(void) {
  atomic_flag_clear_explicit(&h2_wolfssl_lifecycle, memory_order_release);
}

/* Called only by the thread that claimed BUSY from READY/idle or DRAINING/idle.
 * Never hold the bookkeeping lock across provider callbacks or opaque cleanup.
 */
static void cleanup(void) {
  (void)wolfSSL_Cleanup();
  h2_wolfcrypt_crypto_deinit();
  (void)wolfSSL_SetAllocators(h2_wolfssl_state.previous_malloc,
                              h2_wolfssl_state.previous_free,
                              h2_wolfssl_state.previous_realloc);
  memset(&h2_wolfssl_state, 0, sizeof(h2_wolfssl_state));
  memset(&h2_wolfssl_mem_api, 0, sizeof(h2_wolfssl_mem_api));
  atomic_store(&h2_wolfssl_usage, 0u);
}

int h2_wolfssl_entropy_fill(uint8_t* out, size_t len) {
  if (!h2_wolfssl_state.ready || h2_wolfssl_state.entropy == NULL) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  if (out == NULL && len != 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_wolfssl_state.entropy(h2_wolfssl_state.entropy_user, out, len);
}

int h2_wolfssl_is_ready(void) {
  size_t phase = atomic_load(&h2_wolfssl_usage) & H2_WOLFSSL_PHASE;
  /* Existing leased DTLS sessions still need their API to destroy themselves
   * while draining; acquisition below rejects any new session or operation. */
  return phase == H2_WOLFSSL_READY || phase == H2_WOLFSSL_DRAINING;
}

static h2_pal_result_t operation_acquire(int concurrent) {
  size_t usage = atomic_load(&h2_wolfssl_usage);
  for (;;) {
    if ((usage & H2_WOLFSSL_PHASE) != H2_WOLFSSL_READY) {
      return H2_PAL_ERR_INVALID_STATE;
    }
    if (concurrent && (usage & H2_WOLFSSL_CONCURRENT) == 0u) {
      return H2_PAL_ERR_UNSUPPORTED;
    }
    if ((usage & H2_WOLFSSL_COUNT) == H2_WOLFSSL_COUNT) {
      return H2_PAL_ERR_FULL;
    }
    if (atomic_compare_exchange_weak(&h2_wolfssl_usage, &usage, usage + 1u)) {
      return H2_PAL_OK;
    }
  }
}

h2_pal_result_t h2_wolfssl_operation_acquire(void) {
  return operation_acquire(0);
}

h2_pal_result_t h2_wolfssl_operation_acquire_concurrent(void) {
  return operation_acquire(1);
}

void h2_wolfssl_operation_release(void) {
  size_t usage = atomic_load(&h2_wolfssl_usage);
  for (;;) {
    if ((usage & H2_WOLFSSL_COUNT) == 0u) return;
    if (atomic_compare_exchange_weak(&h2_wolfssl_usage, &usage, usage - 1u)) {
      break;
    }
  }
  if ((usage & H2_WOLFSSL_PHASE) == H2_WOLFSSL_DRAINING &&
      (usage & H2_WOLFSSL_COUNT) == 1u) {
    size_t idle = usage - 1u;
    if (atomic_compare_exchange_strong(&h2_wolfssl_usage, &idle,
                                       H2_WOLFSSL_BUSY)) {
      cleanup();
    }
  }
}

h2_pal_result_t h2_wolfssl_session_acquire(void) {
  return h2_wolfssl_operation_acquire();
}

void h2_wolfssl_session_release(void) { h2_wolfssl_operation_release(); }

h2_pal_result_t h2_wolfssl_init(const h2_wolfssl_config_t* config) {
  h2_wolfcrypt_crypto_config_t crypto_config;
  int wolf_rc;

  if (config == NULL || config->mem.vtable == NULL ||
      config->mem.vtable->alloc == NULL ||
      config->mem.vtable->realloc == NULL || config->mem.vtable->free == NULL ||
      config->entropy == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  lifecycle_lock();
  size_t usage = atomic_load(&h2_wolfssl_usage);
  int pinned = 0;
  while ((usage & H2_WOLFSSL_PHASE) == H2_WOLFSSL_DRAINING) {
    if ((usage & H2_WOLFSSL_COUNT) == H2_WOLFSSL_COUNT) {
      lifecycle_unlock();
      return H2_PAL_ERR_FULL;
    }
    /* Pin before reading identity: the last release can otherwise claim
     * cleanup and clear the borrowed provider fields beneath this caller. */
    if (atomic_compare_exchange_weak(&h2_wolfssl_usage, &usage, usage + 1u)) {
      ++usage;
      pinned = 1;
      break;
    }
  }
  if (pinned || (usage & H2_WOLFSSL_PHASE) == H2_WOLFSSL_READY) {
    if (config->mem.user != h2_wolfssl_mem_api.user ||
        config->mem.vtable != h2_wolfssl_mem_api.vtable ||
        config->entropy_user != h2_wolfssl_state.entropy_user ||
        config->entropy != h2_wolfssl_state.entropy ||
        config->providers_thread_safe !=
            ((usage & H2_WOLFSSL_CONCURRENT) != 0u)) {
      lifecycle_unlock();
      if (pinned) h2_wolfssl_operation_release();
      return H2_PAL_ERR_INVALID_STATE;
    }
    if (h2_wolfssl_state.owner_refs == SIZE_MAX) {
      lifecycle_unlock();
      if (pinned) h2_wolfssl_operation_release();
      return H2_PAL_ERR_FULL;
    }
    ++h2_wolfssl_state.owner_refs;
    if (pinned) {
      size_t ready;
      do {
        ready = (usage & ~H2_WOLFSSL_PHASE) | H2_WOLFSSL_READY;
      } while (!atomic_compare_exchange_weak(&h2_wolfssl_usage, &usage, ready));
      /* READY and the real owner prevent cleanup when this pin is dropped. */
      h2_wolfssl_operation_release();
    }
    lifecycle_unlock();
    return H2_PAL_OK;
  }
  if (usage != 0u) {
    lifecycle_unlock();
    return H2_PAL_ERR_INVALID_STATE;
  }
  atomic_store(&h2_wolfssl_usage, H2_WOLFSSL_BUSY);
  lifecycle_unlock();

  memset(&h2_wolfssl_state, 0, sizeof(h2_wolfssl_state));
  h2_wolfssl_mem_api = config->mem;
  h2_wolfssl_state.entropy_user = config->entropy_user;
  h2_wolfssl_state.entropy = config->entropy;
  h2_wolfssl_state.owner_refs = 1u;
  h2_wolfssl_state.ready = 1;

  wolf_rc = wolfSSL_GetAllocators(&h2_wolfssl_state.previous_malloc,
                                  &h2_wolfssl_state.previous_free,
                                  &h2_wolfssl_state.previous_realloc);
  if (wolf_rc != 0 || wolfSSL_SetAllocators(h2_wolfssl_alloc, h2_wolfssl_free,
                                            h2_wolfssl_realloc) != 0) {
    goto fail;
  }

  crypto_config.entropy_user = config->entropy_user;
  crypto_config.entropy = config->entropy;
  if (h2_wolfcrypt_crypto_init(&crypto_config) != H2_PAL_OK) {
    goto restore_allocators;
  }
  if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
    h2_wolfcrypt_crypto_deinit();
    goto restore_allocators;
  }
  atomic_store(
      &h2_wolfssl_usage,
      H2_WOLFSSL_READY |
          (config->providers_thread_safe ? H2_WOLFSSL_CONCURRENT : 0u));
  return H2_PAL_OK;

restore_allocators:
  (void)wolfSSL_SetAllocators(h2_wolfssl_state.previous_malloc,
                              h2_wolfssl_state.previous_free,
                              h2_wolfssl_state.previous_realloc);
fail:
  memset(&h2_wolfssl_state, 0, sizeof(h2_wolfssl_state));
  memset(&h2_wolfssl_mem_api, 0, sizeof(h2_wolfssl_mem_api));
  atomic_store(&h2_wolfssl_usage, 0u);
  return H2_PAL_ERR_IO;
}

h2_pal_result_t h2_wolfssl_deinit(void) {
  lifecycle_lock();
  size_t usage = atomic_load(&h2_wolfssl_usage);
  if (usage == 0u) {
    lifecycle_unlock();
    return H2_PAL_OK;
  }
  if ((usage & H2_WOLFSSL_PHASE) != H2_WOLFSSL_READY) {
    lifecycle_unlock();
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (h2_wolfssl_state.owner_refs > 1u) {
    --h2_wolfssl_state.owner_refs;
    lifecycle_unlock();
    return H2_PAL_OK;
  }
  if ((usage & H2_WOLFSSL_COUNT) != 0u ||
      !atomic_compare_exchange_strong(&h2_wolfssl_usage, &usage,
                                      H2_WOLFSSL_BUSY)) {
    lifecycle_unlock();
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_wolfssl_state.owner_refs = 0u;
  lifecycle_unlock();
  cleanup();
  return H2_PAL_OK;
}

void h2_wolfssl_deinit_deferred(void) {
  lifecycle_lock();
  size_t usage = atomic_load(&h2_wolfssl_usage);
  if ((usage & H2_WOLFSSL_PHASE) != H2_WOLFSSL_READY) {
    lifecycle_unlock();
    return;
  }
  if (--h2_wolfssl_state.owner_refs != 0u) {
    lifecycle_unlock();
    return;
  }
  for (;;) {
    size_t next = (usage & H2_WOLFSSL_COUNT) == 0u
                      ? H2_WOLFSSL_BUSY
                      : (usage & ~H2_WOLFSSL_PHASE) | H2_WOLFSSL_DRAINING;
    if (atomic_compare_exchange_weak(&h2_wolfssl_usage, &usage, next)) {
      lifecycle_unlock();
      if (next == H2_WOLFSSL_BUSY) cleanup();
      return;
    }
  }
}

const h2_pal_crypto_api_t* h2_wolfssl_crypto_api(void) {
  return h2_wolfssl_is_ready() ? h2_wolfcrypt_crypto_api()
                               : h2_pal_unsupported_crypto_api();
}

extern const h2_pal_dtls_api_t h2_wolfssl_dtls_provider;

const h2_pal_dtls_api_t* h2_wolfssl_dtls_api(void) {
  return h2_wolfssl_is_ready() ? &h2_wolfssl_dtls_provider
                               : h2_pal_unsupported_dtls_api();
}
