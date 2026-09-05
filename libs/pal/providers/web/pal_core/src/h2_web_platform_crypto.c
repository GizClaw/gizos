#include "h2_web_platform_internal.h"

#include "h2_wolfcrypt_crypto.h"

#include <emscripten.h>

static size_t h2_web_crypto_users;

EM_JS(int, h2_web_crypto_entropy_js, (uint8_t *out, size_t len), {
  if (!globalThis.crypto ||
      typeof globalThis.crypto.getRandomValues !== 'function') {
    return -3;
  }
  try {
    let offset = 0;
    while (offset < len) {
      const count = Math.min(len - offset, 65536);
      globalThis.crypto.getRandomValues(HEAPU8.subarray(out + offset,
                                                        out + offset + count));
      offset += count;
    }
    return 0;
  } catch (_) {
    return -4;
  }
});

static int h2_web_crypto_entropy(void *user, uint8_t *out, size_t len) {
  (void)user;
  if (out == NULL && len != 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_web_crypto_entropy_js(out, len);
}

int h2_web_platform_crypto_init(h2_web_platform_t *platform) {
  if (platform == NULL || platform->crypto_ready) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (h2_web_crypto_users == 0u) {
    const h2_wolfcrypt_crypto_config_t config = {
        .entropy_user = NULL,
        .entropy = h2_web_crypto_entropy,
    };
    const int rc = h2_wolfcrypt_crypto_init(&config);
    if (rc != H2_PAL_OK) {
      return rc;
    }
  }
  ++h2_web_crypto_users;
  platform->crypto_ready = true;
  return H2_PAL_OK;
}

void h2_web_platform_crypto_deinit(h2_web_platform_t *platform) {
  if (platform == NULL || !platform->crypto_ready) {
    return;
  }
  platform->crypto_ready = false;
  if (h2_web_crypto_users > 0u) {
    --h2_web_crypto_users;
  }
  if (h2_web_crypto_users == 0u) {
    h2_wolfcrypt_crypto_deinit();
  }
}

const h2_pal_crypto_api_t *
h2_web_platform_crypto_api(h2_web_platform_t *platform) {
  if (platform == NULL || !platform->crypto_ready) {
    return h2_pal_unsupported_crypto_api();
  }
  return h2_wolfcrypt_crypto_api();
}
