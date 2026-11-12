#include "h2_alipay_storage.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"

#include <assert.h>
#include <string.h>

static void read_value(const h2_alipay_storage_t* storage, uint32_t index,
                       const void* expected, uint32_t length) {
  uint8_t data[64];
  uint32_t size = sizeof(data);
  assert(h2_alipay_storage_read(storage, index, data, &size) == H2_PAL_OK);
  assert(size == length && memcmp(data, expected, length) == 0);
}

static void certificate_value(const h2_alipay_storage_t* storage,
                              const void* expected, size_t expected_size) {
  h2_pal_pref_namespace_t* ns = NULL;
  const char* name = storage->certificate_namespace != NULL
                         ? storage->certificate_namespace
                         : "alipay_cert";
  assert(h2_pal_pref_open(storage->pref, name, H2_PAL_PREF_OPEN_READ_ONLY,
                          &ns) == H2_PAL_OK);
  void* data = NULL;
  size_t size = 0;
  assert(ns->get_blob(ns, storage->mem, "certificate", &data, &size) ==
         H2_PAL_OK);
  assert(size == expected_size && memcmp(data, expected, size) == 0);
  h2_pal_mem_free(storage->mem, data);
  assert(ns->close(ns) == H2_PAL_OK);
}

static void storage_contract(h2_alipay_storage_t* storage) {
  static const uint8_t first[] = {0, 1, 0xff, 3};
  static const uint8_t replacement[] = {9, 0, 7};
  assert(h2_alipay_storage_exists(storage, 7) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_alipay_storage_write(storage, 7, first, sizeof(first)) ==
         H2_PAL_OK);
  assert(h2_alipay_storage_exists(storage, 7) == 0);
  read_value(storage, 7, first, sizeof(first));
  uint8_t short_buffer[] = {0xaa, 0xbb};
  uint32_t size = sizeof(short_buffer);
  assert(h2_alipay_storage_read(storage, 7, short_buffer, &size) ==
         H2_PAL_ERR_NO_SPACE);
  assert(size == sizeof(first) && short_buffer[0] == 0xaa &&
         short_buffer[1] == 0xbb);
  assert(h2_alipay_storage_write(storage, 7, replacement,
                                 sizeof(replacement)) == H2_PAL_OK);
  read_value(storage, 7, replacement, sizeof(replacement));
  assert(h2_alipay_storage_write(storage, UINT32_MAX, first, sizeof(first)) ==
         H2_PAL_OK);
  read_value(storage, UINT32_MAX, first, sizeof(first));
  assert(h2_alipay_storage_delete(storage, 7) == H2_PAL_OK);
  assert(h2_alipay_storage_exists(storage, 7) == H2_PAL_ERR_NOT_FOUND);
  read_value(storage, UINT32_MAX, first, sizeof(first));
  assert(h2_alipay_storage_save_certificate(storage, first, sizeof(first)) ==
         H2_PAL_OK);
  assert(h2_alipay_storage_save_certificate(storage, replacement,
                                            sizeof(replacement)) == H2_PAL_OK);
  assert(h2_alipay_storage_reset(storage) == H2_PAL_OK);
  assert(h2_alipay_storage_exists(storage, UINT32_MAX) == H2_PAL_ERR_NOT_FOUND);
  certificate_value(storage, replacement, sizeof(replacement));
  assert(h2_alipay_storage_write(storage, 0, NULL, 0) == H2_PAL_OK);
  read_value(storage, 0, "", 0);
  assert(h2_alipay_storage_read(storage, 0, NULL, &size) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_storage_read(storage, 0, short_buffer, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_storage_write(storage, 0, NULL, 1) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_storage_save_certificate(storage, NULL, 0) ==
         H2_PAL_ERR_INVALID_ARG);
}

static void namespace_isolation(h2_app_test_pref_t* pref,
                                const h2_pal_mem_api_t* mem) {
  h2_alipay_storage_t legacy = {.pref = &pref->api, .mem = mem};
  h2_alipay_storage_t sn = {
      .pref = &pref->api,
      .mem = mem,
      .psa_namespace = "alipay_sn_psa",
      .certificate_namespace = "alipay_sn_cert",
  };
  const uint8_t original[] = {1, 2, 3};
  const uint8_t current[] = {4, 5};
  assert(h2_alipay_storage_write(&legacy, 8, original, sizeof(original)) ==
         H2_PAL_OK);
  assert(h2_alipay_storage_save_certificate(&legacy, original,
                                            sizeof(original)) == H2_PAL_OK);
  assert(h2_alipay_storage_exists(&sn, 8) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_alipay_storage_write(&sn, 8, current, sizeof(current)) ==
         H2_PAL_OK);
  assert(h2_alipay_storage_save_certificate(&sn, current, sizeof(current)) ==
         H2_PAL_OK);
  read_value(&legacy, 8, original, sizeof(original));
  read_value(&sn, 8, current, sizeof(current));
  certificate_value(&legacy, original, sizeof(original));
  certificate_value(&sn, current, sizeof(current));
  pref->commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_WRITE, .remaining = 1};
  assert(h2_alipay_storage_write(&sn, 8, original, sizeof(original)) ==
         H2_PAL_ERR_WRITE);
  read_value(&sn, 8, current, sizeof(current));
  pref->commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_WRITE, .remaining = 1};
  assert(h2_alipay_storage_delete(&sn, 8) == H2_PAL_ERR_WRITE);
  read_value(&sn, 8, current, sizeof(current));
  pref->commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_WRITE, .remaining = 1};
  assert(h2_alipay_storage_save_certificate(&sn, original, sizeof(original)) ==
         H2_PAL_ERR_WRITE);
  certificate_value(&sn, current, sizeof(current));
  pref->commit = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1};
  assert(h2_alipay_storage_reset(&sn) == H2_PAL_ERR_IO);
  read_value(&sn, 8, current, sizeof(current));
  storage_contract(&sn);
  assert(h2_alipay_storage_exists(&sn, 8) == H2_PAL_ERR_NOT_FOUND);
  read_value(&legacy, 8, original, sizeof(original));
  certificate_value(&legacy, original, sizeof(original));
  assert(h2_alipay_storage_write(&sn, 8, current, sizeof(current)) ==
         H2_PAL_OK);
  assert(h2_alipay_storage_reset(&legacy) == H2_PAL_OK);
  read_value(&sn, 8, current, sizeof(current));
  certificate_value(&legacy, original, sizeof(original));
  certificate_value(&sn, "\x09\x00\x07", 3);
  const char* invalid_names[][2] = {
      {"", "alipay_sn_cert"},
      {"alipay_sn_psa", ""},
      {"alipay_sn_psa", "alipay_sn_psa"},
      {"alipay_cert", NULL},
      {NULL, "alipay_psa"},
  };
  for (size_t index = 0; index < sizeof(invalid_names) / sizeof(*invalid_names);
       ++index) {
    h2_alipay_storage_t invalid = sn;
    invalid.psa_namespace = invalid_names[index][0];
    invalid.certificate_namespace = invalid_names[index][1];
    assert(h2_alipay_storage_exists(&invalid, 8) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_alipay_storage_save_certificate(
               &invalid, original, sizeof(original)) == H2_PAL_ERR_INVALID_ARG);
  }
}

int main(void) {
  h2_app_test_mem_t mem = {0};
  h2_app_test_mem_init(&mem, NULL);
  h2_app_test_pref_t pref = {0};
  assert(h2_app_test_pref_init(&pref, &mem.api) == H2_PAL_OK);
  h2_alipay_storage_t storage = {.pref = &pref.api, .mem = &mem.api};
  storage_contract(&storage);
  const uint8_t old[] = {1, 2, 3};
  const uint8_t updated[] = {4, 5};
  assert(h2_alipay_storage_write(&storage, 19, old, sizeof(old)) == H2_PAL_OK);
  pref.commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_WRITE, .remaining = 1};
  assert(h2_alipay_storage_write(&storage, 19, updated, sizeof(updated)) ==
         H2_PAL_ERR_WRITE);
  read_value(&storage, 19, old, sizeof(old));
  pref.commit = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1};
  assert(h2_alipay_storage_reset(&storage) == H2_PAL_ERR_IO);
  read_value(&storage, 19, old, sizeof(old));
  pref.open = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1};
  assert(h2_alipay_storage_exists(&storage, 19) == H2_PAL_ERR_IO);
  pref.read = (h2_app_test_fault_t){.result = H2_PAL_EXIT, .remaining = 1};
  assert(h2_alipay_storage_exists(&storage, 19) == H2_PAL_ERR_IO);
  h2_pal_pref_namespace_t* factory = NULL;
  assert(h2_pal_pref_open(storage.pref, "factory", H2_PAL_PREF_OPEN_READ_WRITE,
                          &factory) == H2_PAL_OK);
  assert(factory->set_blob(factory, "identity", old, sizeof(old)) == H2_PAL_OK);
  assert(factory->commit(factory) == H2_PAL_OK &&
         factory->close(factory) == H2_PAL_OK);
  assert(h2_alipay_storage_reset(&storage) == H2_PAL_OK);
  certificate_value(&storage, "\x09\x00\x07", 3);
  assert(h2_pal_pref_open(storage.pref, "factory", H2_PAL_PREF_OPEN_READ_ONLY,
                          &factory) == H2_PAL_OK);
  void* data = NULL;
  size_t size = 0;
  assert(factory->get_blob(factory, &mem.api, "identity", &data, &size) ==
         H2_PAL_OK);
  assert(size == sizeof(old) && memcmp(data, old, size) == 0);
  h2_pal_mem_free(&mem.api, data);
  assert(factory->close(factory) == H2_PAL_OK);
  namespace_isolation(&pref, &mem.api);
  assert(pref.active_handles == 0);
  assert(h2_app_test_pref_deinit(&pref) == H2_PAL_OK);
  assert(mem.live_blocks == 0);
  return 0;
}
