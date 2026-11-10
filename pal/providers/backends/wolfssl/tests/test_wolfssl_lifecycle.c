#include "h2_wolfssl.h"
#include "h2_pal_unsupported.h"
#include "h2_wolfcrypt_crypto.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/memory.h>

static void* test_alloc(void* user, size_t len) {
  (void)user;
  return malloc(len);
}

static void* test_realloc(void* user, void* ptr, size_t len) {
  (void)user;
  return realloc(ptr, len);
}

static void test_free(void* user, void* ptr) {
  (void)user;
  free(ptr);
}

static int test_entropy(void* user, uint8_t* out, size_t len) {
  uint32_t* state = user;
  size_t index;
  for (index = 0u; index < len; ++index) {
    *state = *state * 1664525u + 1013904223u;
    out[index] = (uint8_t)(*state >> 24u);
  }
  return H2_PAL_OK;
}

static h2_pal_result_t discard_send(void* user, const uint8_t* data,
                                    size_t len) {
  (void)user;
  (void)data;
  (void)len;
  return H2_PAL_OK;
}

static h2_pal_result_t discard_plaintext(void* user, const uint8_t* data,
                                         size_t len) {
  (void)user;
  (void)data;
  (void)len;
  return H2_PAL_OK;
}

static void test_deferred_owners(
    const h2_wolfssl_config_t* config, const h2_wolfssl_config_t* changed,
    const h2_pal_dtls_session_config_t* session_config) {
  wolfSSL_Malloc_cb original_malloc;
  wolfSSL_Free_cb original_free;
  wolfSSL_Realloc_cb original_realloc;
  assert(wolfSSL_GetAllocators(&original_malloc, &original_free,
                               &original_realloc) == 0);
  h2_wolfssl_deinit_deferred();
  assert(h2_wolfssl_init(config) == H2_PAL_OK);
  assert(!config->providers_thread_safe);
  assert(h2_wolfssl_operation_acquire_concurrent() == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_wolfssl_operation_acquire() == H2_PAL_OK);
  assert(h2_wolfssl_operation_acquire() == H2_PAL_OK);
  /* Ordinary final deinit retains its owner, allowing the same configuration
   * to acquire a second real owner. Deferred releases consume both exactly. */
  assert(h2_wolfssl_deinit() == H2_PAL_ERR_INVALID_STATE);
  assert(h2_wolfssl_init(config) == H2_PAL_OK);
  h2_wolfssl_deinit_deferred();
  assert(h2_wolfssl_operation_acquire() == H2_PAL_OK);
  h2_wolfssl_operation_release();
  h2_wolfssl_deinit_deferred();
  assert(h2_wolfssl_operation_acquire() == H2_PAL_ERR_INVALID_STATE);
  assert(h2_wolfssl_operation_acquire_concurrent() == H2_PAL_ERR_INVALID_STATE);
  assert(h2_wolfssl_init(config) == H2_PAL_OK);
  assert(h2_wolfssl_operation_acquire() == H2_PAL_OK);
  h2_wolfssl_operation_release();
  assert(h2_wolfssl_init(changed) == H2_PAL_ERR_INVALID_STATE);
  h2_wolfssl_deinit_deferred();
  h2_wolfssl_operation_release();
  assert(h2_wolfssl_init(changed) == H2_PAL_ERR_INVALID_STATE);
  h2_wolfssl_operation_release();
  wolfSSL_Malloc_cb restored_malloc;
  wolfSSL_Free_cb restored_free;
  wolfSSL_Realloc_cb restored_realloc;
  assert(wolfSSL_GetAllocators(&restored_malloc, &restored_free,
                               &restored_realloc) == 0);
  assert(original_malloc == restored_malloc && original_free == restored_free &&
         original_realloc == restored_realloc);
  assert(h2_wolfssl_init(changed) == H2_PAL_OK);
  assert(h2_wolfssl_deinit() == H2_PAL_OK);

  /* Existing DTLS handles must remain destroyable after their last owner goes.
   */
  assert(h2_wolfssl_init(config) == H2_PAL_OK);
  const h2_pal_dtls_api_t* dtls = h2_wolfssl_dtls_api();
  h2_pal_dtls_session_t* session = NULL;
  assert(h2_pal_dtls_session_create(dtls, session_config, &session) ==
         H2_PAL_OK);
  h2_wolfssl_deinit_deferred();
  assert(h2_wolfssl_dtls_api() == dtls);
  assert(h2_wolfssl_init(changed) == H2_PAL_ERR_INVALID_STATE);
  h2_pal_dtls_session_destroy(dtls, &session);
  assert(session == NULL);
  assert(h2_wolfssl_dtls_api() == h2_pal_unsupported_dtls_api());
  assert(h2_wolfssl_init(changed) == H2_PAL_OK);
  h2_wolfssl_deinit_deferred();
  assert(h2_wolfssl_crypto_api() == h2_pal_unsupported_crypto_api());
  assert(h2_wolfssl_init(config) == H2_PAL_OK);
  assert(h2_wolfssl_deinit() == H2_PAL_OK);
}

int main(void) {
  static const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .realloc = test_realloc,
      .free = test_free,
  };
  uint32_t entropy_state = 1u;
  uint32_t other_entropy_state = 2u;
  h2_wolfssl_config_t config = {
      .mem =
          {
              .user = NULL,
              .vtable = &mem_vtable,
          },
      .entropy_user = &entropy_state,
      .entropy = test_entropy,
  };
  h2_wolfssl_config_t mismatched_config;
  h2_wolfcrypt_crypto_config_t blocking_crypto_config = {
      .entropy_user = &other_entropy_state,
      .entropy = test_entropy,
  };
  h2_pal_dtls_session_config_t session_config = {
      .role = H2_PAL_DTLS_ROLE_CLIENT,
      .max_datagram_size = 1500u,
      .max_plaintext_size = 2048u,
      .max_pending_output_bytes = 8192u,
      .send = discard_send,
      .plaintext = discard_plaintext,
      .io_user = NULL,
  };
  h2_pal_dtls_session_t* session = NULL;

  assert(h2_wolfssl_crypto_api() != NULL);
  assert(h2_wolfssl_dtls_api() != NULL);
  assert(h2_pal_crypto_random(h2_wolfssl_crypto_api(), NULL, 0u) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(h2_wolfssl_init(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_wolfcrypt_crypto_init(&blocking_crypto_config) == H2_PAL_OK);
  assert(h2_wolfssl_init(&config) == H2_PAL_ERR_IO);
  assert(h2_wolfssl_crypto_api() == h2_pal_unsupported_crypto_api());
  assert(h2_wolfssl_dtls_api() == h2_pal_unsupported_dtls_api());
  h2_wolfcrypt_crypto_deinit();
  assert(h2_wolfssl_init(&config) == H2_PAL_OK);
  assert(!config.providers_thread_safe);
  assert(h2_wolfssl_operation_acquire_concurrent() == H2_PAL_ERR_UNSUPPORTED);
  mismatched_config = config;
  mismatched_config.providers_thread_safe = true;
  assert(h2_wolfssl_init(&mismatched_config) == H2_PAL_ERR_INVALID_STATE);
  mismatched_config = config;
  mismatched_config.entropy_user = &other_entropy_state;
  assert(h2_wolfssl_init(&mismatched_config) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_wolfssl_init(&config) == H2_PAL_OK);
  assert(h2_pal_crypto_random(h2_wolfssl_crypto_api(), NULL, 0u) == H2_PAL_OK);
  assert(h2_pal_dtls_session_create(h2_wolfssl_dtls_api(), &session_config,
                                    &session) == H2_PAL_OK);
  assert(session != NULL);
  assert(h2_wolfssl_deinit() == H2_PAL_OK);
  assert(h2_wolfssl_deinit() == H2_PAL_ERR_INVALID_STATE);
  h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), &session);
  assert(session == NULL);
  assert(h2_wolfssl_deinit() == H2_PAL_OK);
  assert(h2_wolfssl_deinit() == H2_PAL_OK);
  assert(h2_pal_crypto_random(h2_wolfssl_crypto_api(), NULL, 0u) ==
         H2_PAL_ERR_UNSUPPORTED);
  test_deferred_owners(&config, &mismatched_config, &session_config);
  return 0;
}
