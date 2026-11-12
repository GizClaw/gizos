#include "h2_alipay_storage.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int sdk_result(int result) {
  return result <= 0 ? result : H2_PAL_ERR_IO;
}

static int open_namespace(const h2_alipay_storage_t* storage, bool certificate,
                          h2_pal_pref_open_mode_t mode,
                          h2_pal_pref_namespace_t** out) {
  *out = NULL;
  if (storage == NULL || storage->pref == NULL || storage->mem == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const char* psa =
      storage->psa_namespace != NULL ? storage->psa_namespace : "alipay_psa";
  const char* cert = storage->certificate_namespace != NULL
                         ? storage->certificate_namespace
                         : "alipay_cert";
  if (*psa == '\0' || *cert == '\0' || strcmp(psa, cert) == 0)
    return H2_PAL_ERR_INVALID_ARG;
  const char* name = certificate ? cert : psa;
  int rc = h2_pal_pref_open(storage->pref, name, mode, out);
  if (rc == H2_PAL_OK && *out == NULL) return H2_PAL_ERR_INVALID_STATE;
  return sdk_result(rc);
}

static int finish(h2_pal_pref_namespace_t* ns, int rc) {
  int closed = ns->close != NULL ? ns->close(ns) : H2_PAL_ERR_UNSUPPORTED;
  return sdk_result(rc != H2_PAL_OK ? rc : closed);
}

static void index_key(uint32_t index, char key[10]) {
  (void)snprintf(key, 10, "p%08" PRIx32, index);
}

static int get(const h2_alipay_storage_t* storage, uint32_t index, void** data,
               size_t* size) {
  h2_pal_pref_namespace_t* ns;
  char key[10];
  *data = NULL;
  *size = 0;
  int rc = open_namespace(storage, false, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  if (rc != H2_PAL_OK) return rc;
  index_key(index, key);
  rc = ns->get_blob != NULL ? ns->get_blob(ns, storage->mem, key, data, size)
                            : H2_PAL_ERR_UNSUPPORTED;
  return finish(ns, rc);
}

int h2_alipay_storage_read(const h2_alipay_storage_t* storage, uint32_t index,
                           uint8_t* data, uint32_t* size) {
  if (storage == NULL || storage->mem == NULL || data == NULL || size == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  void* value = NULL;
  size_t length = 0;
  int rc = get(storage, index, &value, &length);
  if (rc == H2_PAL_OK) {
    if (length > UINT32_MAX) {
      rc = H2_PAL_ERR_NO_SPACE;
    } else {
      uint32_t capacity = *size;
      *size = (uint32_t)length;
      if (length > capacity)
        rc = H2_PAL_ERR_NO_SPACE;
      else if (length != 0)
        memcpy(data, value, length);
    }
  }
  h2_pal_mem_free(storage->mem, value);
  return rc;
}

static int put(const h2_alipay_storage_t* storage, bool certificate,
               const char* key, const void* data, uint32_t size) {
  if (data == NULL && size != 0) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_pref_namespace_t* ns;
  int rc =
      open_namespace(storage, certificate, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc != H2_PAL_OK) return rc;
  if (ns->set_blob == NULL || ns->commit == NULL)
    return finish(ns, H2_PAL_ERR_UNSUPPORTED);
  rc = ns->set_blob(ns, key, data, size);
  if (rc == H2_PAL_OK) rc = ns->commit(ns);
  return finish(ns, rc);
}

int h2_alipay_storage_write(const h2_alipay_storage_t* storage, uint32_t index,
                            const uint8_t* data, uint32_t size) {
  char key[10];
  index_key(index, key);
  return put(storage, false, key, data, size);
}

int h2_alipay_storage_exists(const h2_alipay_storage_t* storage,
                             uint32_t index) {
  if (storage == NULL || storage->mem == NULL) return H2_PAL_ERR_INVALID_ARG;
  void* value = NULL;
  size_t size = 0;
  int rc = get(storage, index, &value, &size);
  h2_pal_mem_free(storage->mem, value);
  return rc;
}

int h2_alipay_storage_delete(const h2_alipay_storage_t* storage,
                             uint32_t index) {
  h2_pal_pref_namespace_t* ns;
  char key[10];
  int rc = open_namespace(storage, false, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc != H2_PAL_OK) return rc;
  if (ns->remove == NULL || ns->commit == NULL)
    return finish(ns, H2_PAL_ERR_UNSUPPORTED);
  index_key(index, key);
  rc = ns->remove(ns, key);
  if (rc == H2_PAL_OK) rc = ns->commit(ns);
  return finish(ns, rc);
}

int h2_alipay_storage_reset(const h2_alipay_storage_t* storage) {
  h2_pal_pref_namespace_t* ns;
  int rc = open_namespace(storage, false, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc != H2_PAL_OK) return rc;
  if (ns->clear == NULL || ns->commit == NULL)
    return finish(ns, H2_PAL_ERR_UNSUPPORTED);
  rc = ns->clear(ns);
  if (rc == H2_PAL_OK) rc = ns->commit(ns);
  return finish(ns, rc);
}

int h2_alipay_storage_save_certificate(const h2_alipay_storage_t* storage,
                                       const void* data, uint32_t size) {
  if (data == NULL || size == 0) return H2_PAL_ERR_INVALID_ARG;
  return put(storage, true, "certificate", data, size);
}
