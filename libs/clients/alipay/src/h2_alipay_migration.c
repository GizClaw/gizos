#include "h2_alipay_client_internal.h"

#include <stdio.h>
#include <string.h>

enum {
  LEGACY_SHARE = 2u,
  LEGACY_SEED = 4u,
  LEGACY_NICKNAME = 16u,
  LEGACY_LOGIN = 32u,
  LEGACY_BOUND = 64u,
  LEGACY_UID = 66u
};

typedef struct migration_workspace {
  binding_record_t binding;
  h2_pal_p256_keypair_t local, remote;
  uint8_t old_key[32], old_seed[128], first[144], plain[144], iv[16];
  uint8_t payer[32];
} migration_workspace_t;

static int legacy_read(const h2_alipay_client_config_t* config, uint32_t item,
                       void* output, size_t capacity, size_t* length) {
  h2_pal_pref_namespace_t* namespace = NULL;
  int rc = h2_pal_pref_open(config->legacy_pref, config->legacy_pref_namespace,
                            H2_PAL_PREF_OPEN_READ_ONLY, &namespace);
  if (rc != H2_PAL_OK) return rc;
  if (namespace == NULL) return H2_PAL_ERR_INVALID_STATE;
  char key[13];
  (void)snprintf(key, sizeof(key), "old_%08lx", (unsigned long)item);
  void* raw = NULL;
  size_t size = 0u;
  rc = namespace->get_blob != NULL
           ? namespace->get_blob(namespace, config->mem, key, &raw, &size)
           : H2_PAL_ERR_UNSUPPORTED;
  rc = h2_alipay_client_finish_namespace(namespace, rc);
  if (rc == H2_PAL_OK) {
    if (size != 0u && raw == NULL)
      rc = H2_PAL_ERR_FORMAT;
    else if (size > capacity)
      rc = H2_PAL_ERR_NO_SPACE;
    else {
      if (size != 0u && output != NULL) memcpy(output, raw, size);
      *length = size;
    }
  }
  if (raw != NULL) h2_alipay_client_wipe(raw, size);
  h2_pal_mem_free(config->mem, raw);
  return rc;
}

static int legacy_remove(const h2_alipay_client_config_t* config,
                         uint32_t item) {
  h2_pal_pref_namespace_t* namespace = NULL;
  int rc = h2_pal_pref_open(config->legacy_pref, config->legacy_pref_namespace,
                            H2_PAL_PREF_OPEN_READ_WRITE, &namespace);
  if (rc != H2_PAL_OK) return rc;
  if (namespace == NULL) return H2_PAL_ERR_INVALID_STATE;
  char key[13];
  (void)snprintf(key, sizeof(key), "old_%08lx", (unsigned long)item);
  rc = namespace->remove != NULL && namespace->commit != NULL
           ? namespace->remove(namespace, key)
           : H2_PAL_ERR_UNSUPPORTED;
  if (rc == H2_PAL_OK) rc = namespace->commit(namespace);
  return h2_alipay_client_finish_namespace(namespace, rc);
}

int h2_alipay_client_migrate_data(
    const h2_alipay_client_config_t* config,
    h2_pal_p256_private_key_t* pending_private_key, bool* changed) {
  if (pending_private_key == NULL || changed == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *changed = false;
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return rc;
  if (config->legacy_pref == NULL) return H2_PAL_ERR_UNSUPPORTED;
  migration_workspace_t* work = h2_pal_mem_alloc(config->mem, sizeof(*work));
  if (work == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(work, 0, sizeof(*work));
  rc = h2_alipay_client_load_binding(config, &work->binding);
  if (rc == H2_PAL_OK && h2_alipay_client_binding_complete(&work->binding)) {
    rc = H2_PAL_ERR_INVALID_STATE;
    goto done;
  }
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) goto done;
  h2_alipay_client_wipe(&work->binding, sizeof(work->binding));
  uint8_t flag[4];
  size_t size = 0u;
  rc = legacy_read(config, LEGACY_BOUND, flag, sizeof(flag), &size);
  if (rc != H2_PAL_OK) goto done;
  if (size != 4u || flag[0] != 0xa5u || flag[1] != 0u || flag[2] != 0u ||
      flag[3] != 0u) {
    rc = H2_PAL_ERR_NOT_FOUND;
    goto done;
  }
  const uint32_t required[] = {LEGACY_SEED, LEGACY_SHARE, LEGACY_NICKNAME,
                               LEGACY_LOGIN, LEGACY_UID};
  for (size_t index = 0u; index < sizeof(required) / sizeof(*required);
       ++index) {
    size_t ignored = 0u;
    rc = legacy_read(config, required[index], NULL, SIZE_MAX, &ignored);
    if (rc != H2_PAL_OK) goto done;
  }
  rc = h2_pal_crypto_p256_keypair_generate(config->crypto, &work->local);
  if (rc != H2_PAL_OK) goto done;
  *pending_private_key = work->local.private_key;
  rc = h2_pal_crypto_p256_keypair_generate(config->crypto, &work->remote);
  if (rc == H2_PAL_OK)
    rc = h2_pal_crypto_p256_shared_secret(
        config->crypto, &work->remote.private_key, &work->local.public_key,
        work->binding.secret);
  if (rc != H2_PAL_OK) goto done;
  size_t seed_length = 0u, key_length = 0u;
  rc = legacy_read(config, LEGACY_SEED, work->old_seed, sizeof(work->old_seed),
                   &seed_length);
  if (rc == H2_PAL_OK)
    rc = legacy_read(config, LEGACY_SHARE, work->old_key, sizeof(work->old_key),
                     &key_length);
  if (rc == H2_PAL_OK && key_length != sizeof(work->old_key))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK)
    rc = h2_alipay_paycode_iv(config->identity.mac, work->iv);
  size_t first_length = 0u, plain_length = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_crypto_aes_cbc_pkcs7_decrypt(
        config->crypto, work->old_key + 16u, 16u, work->iv, work->old_seed,
        seed_length, work->first, sizeof(work->first), &first_length);
  if (rc == H2_PAL_OK)
    rc = h2_crypto_aes_cbc_pkcs7_decrypt(
        config->crypto, work->old_key, 16u, work->iv, work->first, first_length,
        work->plain, sizeof(work->plain), &plain_length);
  if (rc == H2_PAL_OK)
    rc = h2_crypto_aes_cbc_pkcs7_encrypt(config->crypto, config->mem,
                                         work->binding.secret, 16u, work->iv,
                                         work->plain, plain_length, work->first,
                                         sizeof(work->first), &first_length);
  if (rc == H2_PAL_OK)
    rc = h2_crypto_aes_cbc_pkcs7_encrypt(
        config->crypto, config->mem, work->binding.secret + 16u, 16u, work->iv,
        work->first, first_length, work->binding.seed,
        sizeof(work->binding.seed), &work->binding.seed_length);
  if (rc != H2_PAL_OK) goto done;
  rc = h2_pal_crypto_p256_shared_secret(config->crypto, pending_private_key,
                                        &work->remote.public_key,
                                        work->binding.secret);
  h2_alipay_client_wipe(pending_private_key, sizeof(*pending_private_key));
  if (rc == H2_PAL_OK)
    rc = legacy_read(config, LEGACY_NICKNAME, work->binding.nickname,
                     BINDING_TEXT_MAX, &work->binding.nickname_length);
  if (rc == H2_PAL_OK)
    rc = legacy_read(config, LEGACY_LOGIN, work->binding.login,
                     BINDING_TEXT_MAX, &work->binding.login_length);
  size_t uid_length = 0u;
  if (rc == H2_PAL_OK)
    rc = legacy_read(config, LEGACY_UID, work->binding.uid, BINDING_ID_MAX,
                     &uid_length);
  if (rc == H2_PAL_OK && (uid_length == 0u || work->binding.uid[0] == '\0'))
    rc = H2_PAL_ERR_FORMAT;
  if (rc != H2_PAL_OK) goto done;
  work->binding.phase = BINDING_MIGRATED;
  rc = h2_alipay_client_store_binding(config, &work->binding);
  if (rc != H2_PAL_OK) goto done;
  *changed = true;
  h2_alipay_client_stage_t stage;
  size_t payer_length = 0u;
  rc = h2_alipay_paycode_generate(config, &work->binding, work->payer,
                                  sizeof(work->payer), &payer_length, &stage);
  if (rc == H2_PAL_OK &&
      (payer_length == 0u || strlen((const char*)work->payer) != payer_length))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) {
    const uint32_t remove[] = {LEGACY_BOUND,    LEGACY_SEED,  LEGACY_SHARE,
                               LEGACY_NICKNAME, LEGACY_LOGIN, LEGACY_UID};
    for (size_t index = 0u; index < sizeof(remove) / sizeof(*remove); ++index)
      (void)legacy_remove(config, remove[index]);
  }
done:
  h2_alipay_client_wipe(work, sizeof(*work));
  h2_pal_mem_free(config->mem, work);
  return rc;
}
