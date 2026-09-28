#include "h2/pal/h2_pal_unsupported.h"
#include "h2_ios_platform.h"
#include "h2_wolfssl.h"
#include "h2_ios_tls_internal.h"
#include <Security/SecRandom.h>
#include <pthread.h>
static int entropy(void *user, uint8_t *out, size_t len) {
  (void)user;
  if (!out && len)
    return H2_PAL_ERR_INVALID_ARG;
  if (!len)
    return H2_PAL_OK;
  return SecRandomCopyBytes(kSecRandomDefault, len, out) == errSecSuccess
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
}

int h2_ios_tls_acquire(void) {
  const h2_wolfssl_config_t config = {
      .mem = *h2_ios_platform_mem_api(), .entropy = entropy,
  };
  return h2_wolfssl_init(&config);
}
int h2_ios_tls_release(void) { return h2_wolfssl_deinit(); }

static pthread_mutex_t lifecycle = PTHREAD_MUTEX_INITIALIZER;
static int ready;
const h2_pal_crypto_api_t *h2_ios_platform_crypto_api(void) {
  pthread_mutex_lock(&lifecycle);
  if (!ready) {
    ready = h2_ios_tls_acquire() == H2_PAL_OK;
  }
  const h2_pal_crypto_api_t *api =
      ready ? h2_wolfssl_crypto_api() : h2_pal_unsupported_crypto_api();
  pthread_mutex_unlock(&lifecycle);
  return api;
}
void h2_ios_platform_crypto_shutdown(void) {
  pthread_mutex_lock(&lifecycle);
  if (ready && h2_ios_tls_release() == H2_PAL_OK)
    ready = 0;
  pthread_mutex_unlock(&lifecycle);
}
