#include "h2_alipay_e2e.h"
#include "h2_app_test_time.h"

#include <assert.h>
#include <string.h>

typedef struct binding_fixture {
  unsigned initializations, codes, queries;
  unsigned complete_after;
  int snapshot_rc, query_rc;
  bool existing, cancel_now, cancel_after_query;
} binding_fixture_t;

static int prepare(h2_runtime_t* runtime, void* user) {
  (void)runtime;
  (void)user;
  return H2_PAL_OK;
}

static int initialize(void* user, int* sdk_rc, bool* first) {
  binding_fixture_t* fixture = user;
  *first = fixture->initializations++ == 0u;
  *sdk_rc = 0;
  return H2_PAL_OK;
}

static int code(void* user, uint32_t feature, char* out, size_t capacity,
                size_t* length, int* sdk_rc) {
  (void)feature;
  binding_fixture_t* fixture = user;
  ++fixture->codes;
  assert(capacity >= sizeof("test-code"));
  memcpy(out, "test-code", sizeof("test-code"));
  *length = sizeof("test-code") - 1u;
  *sdk_rc = 0;
  return H2_PAL_OK;
}

static int snapshot(void* user, h2_alipay_binding_result_t* out) {
  binding_fixture_t* fixture = user;
  out->state = fixture->existing ? H2_ALIPAY_BINDING_COMPLETE
                                 : H2_ALIPAY_BINDING_UNBOUND;
  return fixture->snapshot_rc;
}

static int query(void* user, h2_alipay_binding_result_t* out) {
  binding_fixture_t* fixture = user;
  ++fixture->queries;
  out->state = fixture->complete_after != 0u &&
                       fixture->queries >= fixture->complete_after
                   ? H2_ALIPAY_BINDING_COMPLETE
                   : H2_ALIPAY_BINDING_PENDING;
  return fixture->query_rc;
}

static bool cancel(void* user) {
  binding_fixture_t* fixture = user;
  return fixture->cancel_now ||
         (fixture->cancel_after_query && fixture->queries != 0u);
}

static h2_alipay_e2e_result_t run(binding_fixture_t* fixture) {
  h2_app_test_time_t time;
  h2_app_test_time_init(&time, 100u);
  h2_runtime_t runtime = {.time = &time.api};
  const h2_alipay_vtable_t vtable = {
      .pre_init = initialize,
      .get_binding_code = code,
      .binding_snapshot = snapshot,
      .query_binding_result = query,
  };
  const h2_alipay_api_t api = {.user = fixture, .vtable = &vtable};
  const h2_alipay_e2e_config_t config = {
      .alipay = &api,
      .prepare = prepare,
      .user = fixture,
      .complete_binding = true,
      .binding_timeout_ms = 5000u,
      .binding_interval_ms = 2000u,
  };
  h2_alipay_e2e_result_t result;
  if (h2_alipay_e2e_execute(&runtime, &config, &result) == H2_PAL_OK &&
      result.stage == H2_ALIPAY_E2E_CODE_READY) {
    assert(h2_alipay_e2e_complete_binding(&runtime, &config, cancel, fixture,
                                          &result) == H2_PAL_ERR_INVALID_STATE);
    result.code_presented = true;
    (void)h2_alipay_e2e_complete_binding(&runtime, &config, cancel, fixture,
                                         &result);
  }
  return result;
}

int main(void) {
  binding_fixture_t fixture = {.complete_after = 3u};
  h2_alipay_e2e_result_t result = run(&fixture);
  assert(result.result == H2_PAL_OK &&
         result.stage == H2_ALIPAY_E2E_BINDING_COMPLETE &&
         result.binding_ms == 4000u && result.binding_queries == 3u);
  fixture = (binding_fixture_t){0};
  result = run(&fixture);
  assert(result.result == H2_PAL_ERR_TIMEOUT && result.binding_ms == 5000u &&
         fixture.queries == 3u && fixture.codes == 1u);
  fixture = (binding_fixture_t){.existing = true};
  result = run(&fixture);
  assert(result.stage == H2_ALIPAY_E2E_BINDING_COMPLETE &&
         fixture.codes == 0u && fixture.queries == 0u);
  fixture = (binding_fixture_t){.snapshot_rc = H2_PAL_ERR_INVALID_STATE};
  result = run(&fixture);
  assert(result.result == H2_PAL_ERR_INVALID_STATE && fixture.codes == 0u &&
         fixture.queries == 0u);
  fixture = (binding_fixture_t){.cancel_now = true};
  result = run(&fixture);
  assert(result.result == H2_PAL_ERR_CLOSED && fixture.queries == 0u);
  fixture = (binding_fixture_t){.cancel_after_query = true};
  result = run(&fixture);
  assert(result.result == H2_PAL_ERR_CLOSED && fixture.queries == 1u);
  fixture = (binding_fixture_t){.query_rc = H2_PAL_ERR_WRITE};
  result = run(&fixture);
  assert(result.result == H2_PAL_ERR_WRITE && fixture.queries == 1u);
  return 0;
}
