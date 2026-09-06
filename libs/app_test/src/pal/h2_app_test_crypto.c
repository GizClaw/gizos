#include "h2_app_test_crypto.h"
#include <string.h>
static int random_bytes(void *u, uint8_t *out, size_t len) {
  h2_app_test_crypto_t *c = u;
  if (!out && len)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_app_test_fault_take(&c->random);
  if (rc)
    return rc;
  if (c->random_offset > c->random_size ||
      len > c->random_size - c->random_offset)
    return H2_PAL_ERR_NO_SPACE;
  if (len && !c->random_bytes)
    return H2_PAL_ERR_INVALID_ARG;
  if (len)
    memcpy(out, c->random_bytes + c->random_offset, len);
  c->random_offset += len;
  return 0;
}
static int generate(void *u, h2_pal_x25519_keypair_t *out) {
  h2_app_test_crypto_t *c = u;
  memset(out, 0, sizeof(*out));
  int rc = h2_app_test_fault_take(&c->generate);
  if (rc)
    return rc;
  if (!c->keypair_ready)
    return H2_PAL_ERR_UNSUPPORTED;
  *out = c->keypair;
  return 0;
}
static int derive(void *u, const h2_pal_x25519_private_key_t *key,
                  h2_pal_x25519_public_key_t *out) {
  h2_app_test_crypto_t *c = u;
  memset(out, 0, sizeof(*out));
  int rc = h2_app_test_fault_take(&c->derive);
  if (rc)
    return rc;
  if (!c->keypair_ready)
    return H2_PAL_ERR_UNSUPPORTED;
  if (memcmp(key->bytes, c->keypair.private_key.bytes, sizeof(key->bytes)))
    return H2_PAL_ERR_NOT_FOUND;
  *out = c->keypair.public_key;
  return 0;
}
static const h2_pal_crypto_vtable_t vtable = {
    .random = random_bytes,
    .x25519_keypair_generate = generate,
    .x25519_public_key_from_private = derive};
void h2_app_test_crypto_init(h2_app_test_crypto_t *c) {
  if (!c)
    return;
  memset(c, 0, sizeof(*c));
  c->api = (h2_pal_crypto_api_t){c, &vtable};
}
