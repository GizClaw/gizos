#include "h2/pal/h2_pal_unsupported.h"
#include "h2_android_platform.h"
#include "h2_wolfssl.h"
#include "h2_android_tls_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
static int entropy(void *user, uint8_t *out, size_t len) {
  (void)user;
  if (!out && len)
    return H2_PAL_ERR_INVALID_ARG;
  if (!len)
    return H2_PAL_OK;
  int fd;
  do {
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0)
    return H2_PAL_ERR_IO;
  int rc = H2_PAL_OK;
  while (len) {
    size_t count = len > 256u ? 256u : len;
    ssize_t n = read(fd, out, count);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      rc = H2_PAL_ERR_IO;
      break;
    }
    out += (size_t)n;
    len -= (size_t)n;
  }
  close(fd);
  return rc;
}

int h2_android_tls_acquire(void) {
  const h2_wolfssl_config_t config = {
      .mem = *h2_android_platform_mem_api(), .entropy = entropy,
  };
  return h2_wolfssl_init(&config);
}
int h2_android_tls_release(void) { return h2_wolfssl_deinit(); }

static pthread_mutex_t lifecycle = PTHREAD_MUTEX_INITIALIZER;
static int ready;
const h2_pal_crypto_api_t *h2_android_platform_crypto_api(void) {
  pthread_mutex_lock(&lifecycle);
  if (!ready) {
    ready = h2_android_tls_acquire() == H2_PAL_OK;
  }
  const h2_pal_crypto_api_t *api =
      ready ? h2_wolfssl_crypto_api() : h2_pal_unsupported_crypto_api();
  pthread_mutex_unlock(&lifecycle);
  return api;
}
void h2_android_platform_crypto_shutdown(void) {
  pthread_mutex_lock(&lifecycle);
  if (ready && h2_android_tls_release() == H2_PAL_OK)
    ready = 0;
  pthread_mutex_unlock(&lifecycle);
}
