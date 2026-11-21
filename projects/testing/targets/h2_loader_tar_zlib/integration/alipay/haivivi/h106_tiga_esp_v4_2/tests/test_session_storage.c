#include "h2_alipay_tiga_storage.h"
#include "h2_alipay_tiga_identity.h"
#include "h2_alipay_test_pref.h"
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
  const h2_pal_pref_api_t* persistent = h2_alipay_test_pref_reset();
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
    put(session, "h106_pay_ctx", "value", "PERSISTENT", false);
    assert(h2_alipay_tiga_storage_release() == 0);
    assert(h2_alipay_tiga_storage_configure(&runtime, &session) == 0);
    expect(session, &mem.api, "h106_pay_ctx", "value", "PERSISTENT");
    assert(h2_alipay_tiga_storage_release() == 0);
    assert(h2_alipay_test_pref_idle() && mem.live_blocks == 0u);
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
  put(session, "h106_pay_ctx", "p00000001", "SESSION_KEY", false);
  put(session, "h106_pay_ctx", "binding_ready", "SESSION_MARKER", false);
  put(session, "h106_pay_ctx", "binding", "SESSION_CONTEXT", false);
  expect(session, &mem.api, "h106_pay_ctx", "p00000001", "SESSION_KEY");
  expect(persistent, &mem.api, "h106_pay_psa", "p00000001", NULL);
  expect(persistent, &mem.api, "h106_pay_ctx", "binding", NULL);
  put(session, "h106_pay_ctx", "p00000001", "REPLACED", false);
  expect(session, &mem.api, "h106_pay_ctx", "p00000001", "REPLACED");
  expect(session, &mem.api, "h106_pay_ctx", "binding_ready", "SESSION_MARKER");
  h2_pal_pref_namespace_t* ns = NULL;
  assert(h2_pal_pref_open(session, "h106_pay_psa", H2_PAL_PREF_OPEN_READ_WRITE,
                          &ns) == H2_PAL_ERR_UNSUPPORTED &&
         ns == NULL);
  assert(h2_pal_pref_open(session, "unrelated", H2_PAL_PREF_OPEN_READ_WRITE,
                          &ns) == H2_PAL_ERR_UNSUPPORTED &&
         ns == NULL);
  assert(h2_pal_pref_open(session, "h106_pay_ctx", H2_PAL_PREF_OPEN_READ_ONLY,
                          &ns) == 0);
  assert(ns->set_blob(ns, "binding", "X", 1u) == H2_PAL_ERR_INVALID_STATE);
  assert(ns->set_u32(ns, "npay_quick", 1u) == H2_PAL_ERR_INVALID_STATE);
  assert(ns->remove(ns, "binding") == H2_PAL_ERR_INVALID_STATE);
  assert(h2_alipay_tiga_storage_release() == H2_PAL_ERR_INVALID_STATE);
  assert(ns->close(ns) == 0);
  expect(session, &mem.api, "h106_pay_ctx", "binding", "SESSION_CONTEXT");
  assert(h2_pal_pref_open(session, "h106_pay_ctx", H2_PAL_PREF_OPEN_READ_WRITE,
                          &ns) == 0);
  uint32_t flag = 0xa5a5a5a5u;
  assert(ns->get_u32(ns, "npay_quick", &flag) == H2_PAL_ERR_NOT_FOUND);
  assert(flag == 0xa5a5a5a5u);
  const uint32_t flags[] = {0u, 1u, 2u, UINT32_MAX};
  for (size_t index = 0u; index < sizeof(flags) / sizeof(*flags); ++index) {
    assert(ns->set_u32(ns, "npay_quick", flags[index]) == 0);
    assert(ns->get_u32(ns, "npay_quick", &flag) == 0 && flag == flags[index]);
  }
  void* wrong_type = NULL;
  size_t wrong_size = 0u;
  assert(ns->get_blob(ns, &mem.api, "npay_quick", &wrong_type, &wrong_size) ==
         H2_PAL_ERR_FORMAT);
  assert(wrong_type == NULL && wrong_size == 0u);
  assert(ns->get_u32(ns, "binding", &flag) == H2_PAL_ERR_FORMAT);
  assert(flag == UINT32_MAX);
  mem.fail_at = mem.calls + 1u;
  assert(ns->set_u32(ns, "npay_quick", 0u) == H2_PAL_ERR_NO_MEMORY);
  mem.fail_at = 0u;
  assert(ns->get_u32(ns, "npay_quick", &flag) == 0 && flag == UINT32_MAX);
  h2_pal_pref_namespace_t* duplicate = NULL;
  assert(h2_pal_pref_open(session, "h106_pay_ctx", H2_PAL_PREF_OPEN_READ_WRITE,
                          &duplicate) == H2_PAL_ERR_BUSY &&
         duplicate == NULL);
  mem.fail_at = mem.calls + 1u;
  assert(ns->set_blob(ns, "binding", "NEW", 3u) == H2_PAL_ERR_NO_MEMORY);
  mem.fail_at = 0u;
  expect(session, &mem.api, "h106_pay_ctx", "binding", "SESSION_CONTEXT");
  char oversized_key[33];
  memset(oversized_key, 'K', sizeof(oversized_key));
  oversized_key[sizeof(oversized_key) - 1u] = '\0';
  assert(ns->set_blob(ns, oversized_key, "X", 1u) == H2_PAL_ERR_INVALID_ARG);
  assert(ns->set_blob(ns, "binding", "X", 16385u) == H2_PAL_ERR_NO_SPACE);
  assert(ns->remove(ns, "binding") == 0);
  assert(ns->remove(ns, "binding") == H2_PAL_ERR_NOT_FOUND);
  assert(ns->commit(ns) == 0 && ns->close(ns) == 0);
  assert(h2_pal_pref_open(session, "h106_pay_ctx", H2_PAL_PREF_OPEN_READ_ONLY,
                          &ns) == 0);
  assert(ns->get_u32(ns, "npay_quick", &flag) == 0 && flag == UINT32_MAX);
  assert(ns->close(ns) == 0);
  assert(ns->close(ns) == H2_PAL_ERR_INVALID_ARG);
  expect(session, &mem.api, "h106_pay_ctx", "binding", NULL);
  put(session, "h106_pay_ctx", "binding", "SESSION_CONTEXT", false);
  assert(h2_alipay_tiga_storage_audit() == 0);
  const h2_pal_pref_api_t* reused = NULL;
  assert(h2_alipay_tiga_storage_configure(&runtime, &reused) == 0 &&
         reused == session);
  assert(h2_alipay_tiga_storage_release() == 0);
  assert(h2_alipay_tiga_storage_configure(&runtime, &session) == 0);
  expect(session, &mem.api, "h106_pay_ctx", "p00000001", NULL);
  expect(session, &mem.api, "h106_pay_ctx", "binding", NULL);
  assert(h2_pal_pref_open(session, "h106_pay_ctx", H2_PAL_PREF_OPEN_READ_ONLY,
                          &ns) == 0);
  assert(ns->get_u32(ns, "npay_quick", &flag) == H2_PAL_ERR_NOT_FOUND);
  assert(ns->close(ns) == 0);
  put(persistent, "h106_pay_psa", "unexpected", "BAD", false);
  assert(h2_alipay_tiga_storage_audit() == H2_PAL_ERR_INVALID_STATE);
  assert(h2_alipay_tiga_storage_release() == 0);
  assert(h2_alipay_test_pref_idle());
  assert(mem.live_blocks == 0u);
  return 0;
}
