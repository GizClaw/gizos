#include "h2_alipay_e2e.h"
#include <assert.h>
#include <string.h>

typedef struct fixture {
  int prepared, init_calls, ready_calls, status_calls, code_calls;
  int network_rc, sdk_rc, ready_rc, code_rc;
  bool bad_reentry, bound, bad_length;
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
static h2_pal_result_t ready(void* user, uint32_t feature,
                             h2_alipay_blocker_t* blocker) {
  fixture_t* f = user;
  assert(f->init_calls == 2 && feature == 7);
  ++f->ready_calls;
  *blocker =
      f->ready_rc ? H2_ALIPAY_BLOCKER_SECURITY_SCHEME : H2_ALIPAY_BLOCKER_NONE;
  return f->ready_rc;
}
static h2_pal_result_t status(void* user, bool* bound) {
  fixture_t* f = user;
  assert(f->ready_calls == 1 && f->ready_rc == 0);
  ++f->status_calls;
  *bound = f->bound;
  return H2_PAL_OK;
}
static h2_pal_result_t code(void* user, uint32_t feature, char* out,
                            size_t capacity, size_t* length, int* sdk_rc) {
  fixture_t* f = user;
  assert(f->status_calls == 1 && !f->bound && feature == 7);
  ++f->code_calls;
  assert(capacity >= 8);
  memcpy(out, "real://", 8);
  *length = f->bad_length ? capacity : 7;
  *sdk_rc = f->code_rc;
  return f->code_rc ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static const h2_alipay_vtable_t vtable = {
    .pre_init = init,
    .check_ready = ready,
    .binding_status = status,
    .get_binding_code = code,
};
static h2_alipay_e2e_result_t run(fixture_t* fixture) {
  h2_runtime_t runtime = {0};
  const h2_alipay_api_t api = {.user = fixture, .vtable = &vtable};
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
  assert(f.init_calls == 1 && f.ready_calls == 0);

  f = (fixture_t){.bad_reentry = true};
  r = run(&f);
  assert(!r.reused && r.result == H2_PAL_ERR_INVALID_STATE);
  assert(f.ready_calls == 0);

  f = (fixture_t){.ready_rc = H2_PAL_ERR_UNAVAILABLE};
  r = run(&f);
  assert(r.init_sdk_rc == 0 && r.reused);
  assert(r.blocker == H2_ALIPAY_BLOCKER_SECURITY_SCHEME);
  assert(f.status_calls == 0 && f.code_calls == 0);
  assert(r.code_sdk_rc == H2_ALIPAY_E2E_NOT_CALLED && r.length == 0);

  f = (fixture_t){.bound = true};
  r = run(&f);
  assert(r.stage == H2_ALIPAY_E2E_BOUND && f.code_calls == 0);

  f = (fixture_t){.code_rc = -91};
  r = run(&f);
  assert(r.code_sdk_rc == -91 && r.result == H2_PAL_ERR_IO);
  assert(r.length == 0 && r.code[0] == '\0');

  f = (fixture_t){.bad_length = true};
  r = run(&f);
  assert(r.result == H2_PAL_ERR_FORMAT && r.length == 0 && r.code[0] == '\0');

  f = (fixture_t){0};
  r = run(&f);
  assert(r.stage == H2_ALIPAY_E2E_CODE_READY && r.result == H2_PAL_OK);
  assert(r.length == 7 && strcmp(r.code, "real://") == 0);
  assert(!r.qr_visible && r.display_rc == H2_ALIPAY_E2E_NOT_CALLED);
  return 0;
}
