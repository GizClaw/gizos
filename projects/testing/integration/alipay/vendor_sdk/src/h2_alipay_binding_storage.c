#include "h2_alipay_binding_storage.h"

#include "h2_sha256.h"
#include <string.h>

static const char marker_key[] = "binding_ready";

static void wipe(void* data, size_t length) {
  volatile uint8_t* bytes = data;
  while (length-- != 0u) *bytes++ = 0u;
}

void h2_alipay_binding_value_digest(const uint8_t* data, size_t length,
                                    uint8_t out[32]) {
  h2_sha256_t hash;
  uint8_t encoded[8];
  for (size_t byte = 0u; byte < sizeof(encoded); ++byte)
    encoded[byte] = (uint8_t)((uint64_t)length >> (byte * 8u));
  h2_sha256_init(&hash);
  h2_sha256_update(&hash, encoded, sizeof(encoded));
  h2_sha256_update(&hash, data, length);
  h2_sha256_finish(&hash, out);
  wipe(&hash, sizeof(hash));
}

static int open_marker(const h2_alipay_storage_t* storage,
                       h2_pal_pref_open_mode_t mode,
                       h2_pal_pref_namespace_t** out) {
  *out = NULL;
  if (storage == NULL || storage->pref == NULL || storage->mem == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  const char* name =
      storage->psa_namespace != NULL ? storage->psa_namespace : "alipay_psa";
  const char* certificate = storage->certificate_namespace != NULL
                                ? storage->certificate_namespace
                                : "alipay_cert";
  if (*name == '\0' || *certificate == '\0' || strcmp(name, certificate) == 0)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_pal_pref_open(storage->pref, name, mode, out);
  if (rc == H2_PAL_OK && *out == NULL) return H2_PAL_ERR_INVALID_STATE;
  return rc <= 0 ? rc : H2_PAL_ERR_IO;
}

static int close_marker(h2_pal_pref_namespace_t* namespace, int result) {
  int closed = namespace->close != NULL ? namespace->close(namespace)
                                        : H2_PAL_ERR_UNSUPPORTED;
  if (result == H2_PAL_OK) result = closed;
  return result <= 0 ? result : H2_PAL_ERR_IO;
}

static int read_marker(const h2_alipay_storage_t* storage, void** data,
                       size_t* size) {
  *data = NULL;
  *size = 0u;
  h2_pal_pref_namespace_t* namespace;
  int rc = open_marker(storage, H2_PAL_PREF_OPEN_READ_ONLY, &namespace);
  if (rc != H2_PAL_OK) return rc;
  rc =
      namespace->get_blob != NULL
          ? namespace->get_blob(namespace, storage->mem, marker_key, data, size)
          : H2_PAL_ERR_UNSUPPORTED;
  return close_marker(namespace, rc);
}

int h2_alipay_binding_storage_read(const h2_alipay_storage_t* storage,
                                   h2_alipay_binding_result_t* out,
                                   uint8_t digest[32]) {
  if (storage == NULL || out == NULL || digest == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  out->tid_length = out->uid_length = out->shared_key_length =
      out->seed_length = 0u;
  memset(digest, 0, 32u);
  const uint32_t items[] = {
      H2_ALIPAY_SDK_ITEM_TID,
      H2_ALIPAY_SDK_ITEM_UID,
      H2_ALIPAY_SDK_ITEM_SHARED_KEY,
      H2_ALIPAY_SDK_ITEM_SEED,
  };
  size_t* lengths[] = {&out->tid_length, &out->uid_length,
                       &out->shared_key_length, &out->seed_length};
  h2_sha256_t hash;
  h2_sha256_init(&hash);
  uint8_t data[256], value_digest[32];
  int rc = H2_PAL_OK;
  for (size_t index = 0u; index < 4u; ++index) {
    uint32_t length = sizeof(data);
    rc = h2_alipay_storage_read(storage, items[index], data, &length);
    if (rc != H2_PAL_OK) break;
    *lengths[index] = length;
    if (length == 0u ||
        (items[index] == H2_ALIPAY_SDK_ITEM_SHARED_KEY && length != 32u) ||
        (items[index] == H2_ALIPAY_SDK_ITEM_SEED &&
         (length < 16u || length > 128u))) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    h2_alipay_binding_value_digest(data, length, value_digest);
    h2_sha256_update(&hash, value_digest, sizeof(value_digest));
    wipe(data, sizeof(data));
  }
  if (rc == H2_PAL_OK) h2_sha256_finish(&hash, digest);
  wipe(data, sizeof(data));
  wipe(value_digest, sizeof(value_digest));
  wipe(&hash, sizeof(hash));
  return rc;
}

int h2_alipay_binding_storage_verify(const h2_alipay_storage_t* storage,
                                     const uint8_t digest[32]) {
  if (storage == NULL || digest == NULL) return H2_PAL_ERR_INVALID_ARG;
  void* data = NULL;
  size_t size = 0u;
  int rc = read_marker(storage, &data, &size);
  if (rc == H2_PAL_OK &&
      (size != 36u || data == NULL || memcmp(data, "ABV1", 4u) != 0 ||
       memcmp((const uint8_t*)data + 4u, digest, 32u) != 0))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (data != NULL) wipe(data, size);
  h2_pal_mem_free(storage->mem, data);
  return rc;
}

int h2_alipay_binding_storage_mark(const h2_alipay_storage_t* storage,
                                   const uint8_t digest[32]) {
  if (storage == NULL || digest == NULL) return H2_PAL_ERR_INVALID_ARG;
  uint8_t data[36];
  memcpy(data, "ABV1", 4u);
  memcpy(data + 4u, digest, 32u);
  h2_pal_pref_namespace_t* namespace;
  int rc = open_marker(storage, H2_PAL_PREF_OPEN_READ_WRITE, &namespace);
  if (rc != H2_PAL_OK) return rc;
  if (namespace->set_blob == NULL || namespace->commit == NULL)
    return close_marker(namespace, H2_PAL_ERR_UNSUPPORTED);
  rc = namespace->set_blob(namespace, marker_key, data, sizeof(data));
  if (rc == H2_PAL_OK) rc = namespace->commit(namespace);
  rc = close_marker(namespace, rc);
  wipe(data, sizeof(data));
  if (rc == H2_PAL_OK) rc = h2_alipay_binding_storage_verify(storage, digest);
  return rc;
}

int h2_alipay_binding_storage_invalidate(const h2_alipay_storage_t* storage) {
  void* data = NULL;
  size_t size = 0u;
  int rc = read_marker(storage, &data, &size);
  if (data != NULL) wipe(data, size);
  h2_pal_mem_free(storage != NULL ? storage->mem : NULL, data);
  if (rc == H2_PAL_ERR_NOT_FOUND) return H2_PAL_OK;
  if (rc != H2_PAL_OK) return rc;
  h2_pal_pref_namespace_t* namespace;
  rc = open_marker(storage, H2_PAL_PREF_OPEN_READ_WRITE, &namespace);
  if (rc != H2_PAL_OK) return rc;
  if (namespace->remove == NULL || namespace->commit == NULL)
    return close_marker(namespace, H2_PAL_ERR_UNSUPPORTED);
  rc = namespace->remove(namespace, marker_key);
  if (rc == H2_PAL_OK) rc = namespace->commit(namespace);
  return close_marker(namespace, rc);
}

int h2_alipay_binding_storage_probe(const h2_alipay_storage_t* storage,
                                    bool* record_present) {
  if (record_present == NULL) return H2_PAL_ERR_INVALID_ARG;
  *record_present = false;
  void* data = NULL;
  size_t size = 0u;
  int rc = read_marker(storage, &data, &size);
  if (data != NULL) wipe(data, size);
  h2_pal_mem_free(storage != NULL ? storage->mem : NULL, data);
  if (rc == H2_PAL_ERR_NOT_FOUND) return H2_PAL_OK;
  if (rc == H2_PAL_OK) *record_present = true;
  return rc;
}
