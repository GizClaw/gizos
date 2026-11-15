#include "h2_alipay_tiga_storage.h"
#include "h2_alipay_tiga_identity.h"
#include "h2_memory_pref.h"
#include "h2_app_test_mem.h"

#include <assert.h>
#include <string.h>

static void put(const h2_pal_pref_api_t* api, const char* name, const char* key,
                const char* value, bool string) {
  h2_pal_pref_namespace_t* ns = NULL;
  assert(h2_pal_pref_open(api, name, H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  assert((string ? ns->set_string(ns, key, value)
                 : ns->set_blob(ns, key, value, strlen(value))) == 0);
  assert(ns->commit(ns) == 0 && ns->close(ns) == 0);
}
static void expect(const h2_pal_pref_api_t* api, const h2_pal_mem_api_t* mem,
                   const char* name, const char* key, const char* value) {
  h2_pal_pref_namespace_t* ns = NULL;
  void* data = NULL;
  size_t size = 0u;
  int rc = h2_pal_pref_open(api, name, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  if (rc == 0) {
    rc = ns->get_blob(ns, mem, key, &data, &size);
    assert(ns->close(ns) == 0);
  }
  if (value == NULL)
    assert(rc == H2_PAL_ERR_NOT_FOUND && data == NULL);
  else
    assert(rc == 0 && size == strlen(value) && memcmp(data, value, size) == 0);
  h2_pal_mem_free(mem, data);
}
static int fail_commit(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  return H2_PAL_ERR_WRITE;
}
static int failing_open(void* user, const char* name,
                        h2_pal_pref_open_mode_t mode,
                        h2_pal_pref_namespace_t** out) {
  int rc = h2_pal_pref_open(user, name, mode, out);
  if (rc == 0 && mode == H2_PAL_PREF_OPEN_READ_WRITE &&
      strcmp(name, "h106_pay_psa") == 0)
    (*out)->commit = fail_commit;
  return rc;
}

int main(void) {
  h2_app_test_mem_t mem;
  h2_app_test_mem_init(&mem, NULL);
  const h2_memory_pref_config_t config = {&mem.api, 20u, 16u, 4096u, 8u};
  h2_memory_pref_t* durable_fixture = NULL;
  assert(h2_memory_pref_create(&config, &durable_fixture) == 0);
  const h2_pal_pref_api_t* persistent = h2_memory_pref_api(durable_fixture);
  h2_runtime_t runtime = {.mem = &mem.api, .pref = persistent};
  const h2_pal_pref_api_t* session = NULL;
  const char* legacy[] = {"alipay_psa",   "alipay_sn_psa", "alipay_h106_psa",
                          "h106_pay_psa", "alipay_direct", "alipay_h106_ctx",
                          "h106_pay_ctx"};
  for (size_t i = 0u; i < 7u; ++i)
    put(persistent, legacy[i], "old", "OLD_BINDING", false);
  if (h2_alipay_tiga_storage_scope() == H2_ALIPAY_STORAGE_PERSISTENT) {
    assert(h2_alipay_tiga_storage_configure(&runtime, &session) == 0 &&
           session == persistent);
    for (size_t i = 0u; i < 7u; ++i)
      expect(persistent, &mem.api, legacy[i], "old", "OLD_BINDING");
    put(session, "h106_pay_psa", "value", "PERSISTENT", false);
    assert(h2_alipay_tiga_storage_release() == 0);
    assert(h2_alipay_tiga_storage_configure(&runtime, &session) == 0);
    expect(session, &mem.api, "h106_pay_psa", "value", "PERSISTENT");
    assert(h2_alipay_tiga_storage_release() == 0);
    assert(h2_memory_pref_destroy(&durable_fixture) == 0 &&
           mem.live_blocks == 0u);
    return 0;
  }
  put(persistent, "h106_factory", "imei", "INVALID_SN", true);
  assert(h2_alipay_tiga_storage_configure(&runtime, &session) ==
             H2_PAL_ERR_FORMAT &&
         session == NULL);
  expect(persistent, &mem.api, "h106_pay_psa", "old", "OLD_BINDING");
  put(persistent, "h106_factory", "imei", "490154203237518",
      true); /* synthetic public Luhn example */
  const char* certs[] = {"alipay_cert", "alipay_sn_cert", "alipay_h106_crt",
                         "h106_pay_cert"};
  for (size_t i = 0u; i < 4u; ++i)
    put(persistent, certs[i], "certificate", "CERT_SENTINEL", false);
  put(persistent, "unrelated", "value", "KEEP", false);
  static const h2_pal_pref_vtable_t failing = {.open = failing_open};
  h2_pal_pref_api_t failed_commit = {(void*)persistent, &failing};
  runtime.pref = &failed_commit;
  assert(h2_alipay_tiga_storage_configure(&runtime, &session) ==
             H2_PAL_ERR_WRITE &&
         session == NULL);
  expect(persistent, &mem.api, "h106_pay_psa", "old", "OLD_BINDING");
  runtime.pref = persistent;
  assert(h2_alipay_tiga_storage_configure(&runtime, &session) == 0);
  assert(session != persistent &&
         h2_alipay_tiga_storage_scope() == H2_ALIPAY_STORAGE_SESSION);
  for (size_t i = 0u; i < 7u; ++i)
    expect(persistent, &mem.api, legacy[i], "old", NULL);
  for (size_t i = 0u; i < 4u; ++i)
    expect(persistent, &mem.api, certs[i], "certificate", "CERT_SENTINEL");
  expect(persistent, &mem.api, "unrelated", "value", "KEEP");
  char sn[49];
  assert(h2_alipay_tiga_read_sn(&runtime, sn, sizeof(sn)) == 0 &&
         strcmp(sn, "490154203237518") == 0);
  put(session, "h106_pay_psa", "p00000001", "SESSION_KEY", false);
  put(session, "h106_pay_psa", "binding_ready", "SESSION_MARKER", false);
  put(session, "h106_pay_ctx", "binding", "SESSION_CONTEXT", false);
  expect(session, &mem.api, "h106_pay_psa", "p00000001", "SESSION_KEY");
  expect(persistent, &mem.api, "h106_pay_psa", "p00000001", NULL);
  expect(persistent, &mem.api, "h106_pay_ctx", "binding", NULL);
  assert(h2_alipay_tiga_storage_audit() == 0);
  const h2_pal_pref_api_t* reused = NULL;
  assert(h2_alipay_tiga_storage_configure(&runtime, &reused) == 0 &&
         reused == session);
  assert(h2_alipay_tiga_storage_release() == 0);
  assert(h2_alipay_tiga_storage_configure(&runtime, &session) == 0);
  expect(session, &mem.api, "h106_pay_psa", "p00000001", NULL);
  expect(session, &mem.api, "h106_pay_ctx", "binding", NULL);
  put(persistent, "h106_pay_psa", "unexpected", "BAD", false);
  assert(h2_alipay_tiga_storage_audit() == H2_PAL_ERR_INVALID_STATE);
  assert(h2_alipay_tiga_storage_release() == 0);
  assert(h2_memory_pref_destroy(&durable_fixture) == 0);
  assert(mem.live_blocks == 0u);
  return 0;
}
