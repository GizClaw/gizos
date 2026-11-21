#include "h2_alipay_e2e.h"
#include <assert.h>
#include <string.h>

typedef struct fixture {
  int prepared, init_calls, code_calls;
  int network_rc, sdk_rc, code_rc;
  bool bad_reentry, bad_length;
} fixture_t;

static h2_pal_result_t prepare(h2_runtime_t* runtime, void* user) {
  (void)runtime;
  fixture_t* f = user;
  ++f->prepared;
  return f->network_rc;
}
static h2_pal_result_t init(void* user, int* sdk_rc, bool* first) {
  fixture_t* f = user;
  assert(f->prepared == 1);
  *first = f->init_calls++ == 0 || f->bad_reentry;
  *sdk_rc = f->sdk_rc;
  return H2_PAL_OK;
}
static h2_pal_result_t code(void* user, uint32_t feature, char* out,
                            size_t capacity, size_t* length, int* sdk_rc) {
  fixture_t* f = user;
  assert(f->init_calls == 2 && feature == 7);
  ++f->code_calls;
  assert(capacity >= 8);
  memcpy(out, "real://", 8);
  *length = f->bad_length ? capacity : 7;
  *sdk_rc = f->code_rc;
  return f->code_rc ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static const h2_alipay_vtable_t vtable = {
    .pre_init = init,
    .get_binding_code = code,
};
static h2_alipay_e2e_result_t run(fixture_t* fixture) {
  h2_runtime_t runtime = {0};
  h2_alipay_vtable_t calls = vtable;
  const h2_alipay_api_t api = {.user = fixture, .vtable = &calls};
  const h2_alipay_e2e_config_t config = {
      .alipay = &api, .feature = 7, .prepare = prepare, .user = fixture};
  h2_alipay_e2e_result_t result;
  (void)h2_alipay_e2e_execute(&runtime, &config, &result);
  return result;
}
int main(void) {
  fixture_t f = {.network_rc = H2_PAL_ERR_TIMEOUT};
  h2_alipay_e2e_result_t r = run(&f);
  assert(r.stage == H2_ALIPAY_E2E_NETWORK && r.result == H2_PAL_ERR_TIMEOUT);
  assert(f.init_calls == 0 && r.init_sdk_rc == H2_ALIPAY_E2E_NOT_CALLED);

  f = (fixture_t){.sdk_rc = 42};
  r = run(&f);
  assert(r.init_sdk_rc == 42 && r.result == H2_PAL_ERR_IO);
  assert(f.init_calls == 1 && f.code_calls == 0);

  f = (fixture_t){.bad_reentry = true};
  r = run(&f);
  assert(!r.reused && r.result == H2_PAL_ERR_INVALID_STATE &&
         f.code_calls == 0);

  f = (fixture_t){0};
  r = run(&f);
  assert(r.stage == H2_ALIPAY_E2E_CODE_READY && r.result == H2_PAL_OK);
  assert(r.init_sdk_rc == 0 && r.reused && f.code_calls == 1);
  assert(r.length == 7 && strcmp(r.code, "real://") == 0);
  assert(!r.qr_visible && !r.code_presented &&
         r.display_rc == H2_ALIPAY_E2E_NOT_CALLED);

  f = (fixture_t){.code_rc = -91};
  r = run(&f);
  assert(r.code_sdk_rc == -91 && r.result == H2_PAL_ERR_IO);
  assert(r.length == 0 && r.code[0] == '\0');

  f = (fixture_t){.bad_length = true};
  r = run(&f);
  assert(r.result == H2_PAL_ERR_FORMAT && r.length == 0 && r.code[0] == '\0');
  return 0;
}
