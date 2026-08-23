#include "h2_gizclaw_e2e_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int s_mutex_storage;

static void *test_alloc(void *user, size_t len) {
  (void)user;
  return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
  (void)user;
  return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static h2_pal_result_t test_random(void *user, uint8_t *out, size_t len) {
  (void)user;
  memset(out, 0x6b, len);
  return H2_PAL_OK;
}

static h2_pal_result_t test_time(void *user, uint64_t *out_ms) {
  (void)user;
  *out_ms = 100u;
  return H2_PAL_OK;
}

static h2_pal_result_t test_mutex_create(
    void *user, const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
  (void)user;
  (void)config;
  *out_mutex = (h2_pal_mutex_t *)&s_mutex_storage;
  return H2_PAL_OK;
}

static h2_pal_result_t test_mutex_operation(void *user,
                                            h2_pal_mutex_t *mutex) {
  (void)user;
  (void)mutex;
  return H2_PAL_OK;
}

static int test_log(void *user, h2_pal_log_level_t level, const char *scope,
                    const char *message) {
  (void)user;
  (void)level;
  (void)scope;
  (void)message;
  return H2_PAL_OK;
}

int main(void) {
  const h2_gizclaw_str_t empty = h2_gizclaw_e2e_str(NULL);
  assert(empty.data == NULL && empty.len == 0u);
  const h2_gizclaw_str_t value = h2_gizclaw_e2e_str("portable");
  assert(value.data != NULL && value.len == strlen("portable"));

  static const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .realloc = test_realloc,
      .free = test_free,
  };
  static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
  static const h2_pal_crypto_vtable_t crypto_vtable = {
      .random = test_random,
  };
  static const h2_pal_crypto_api_t crypto = {.vtable = &crypto_vtable};
  static const h2_pal_http_vtable_t http_vtable = {0};
  static const h2_pal_http_api_t http = {.vtable = &http_vtable};
  static const h2_pal_log_vtable_t log_vtable = {.write = test_log};
  static const h2_pal_log_api_t log = {.vtable = &log_vtable};
  static const h2_pal_time_vtable_t time_vtable = {
      .get_monotonic_ms = test_time,
  };
  static const h2_pal_time_api_t time = {.vtable = &time_vtable};
  static const h2_pal_sync_vtable_t sync_vtable = {
      .create_mutex = test_mutex_create,
      .destroy_mutex = test_mutex_operation,
      .lock_mutex = test_mutex_operation,
      .try_lock_mutex = test_mutex_operation,
      .unlock_mutex = test_mutex_operation,
  };
  static const h2_pal_sync_api_t sync = {.vtable = &sync_vtable};
  static const h2_pal_webrtc_vtable_t webrtc_vtable = {0};
  static const h2_pal_webrtc_api_t webrtc = {.vtable = &webrtc_vtable};
  h2_runtime_t runtime = {
      .mem = &mem,
      .log = &log,
      .time = &time,
      .sync = &sync,
      .crypto = &crypto,
      .http = &http,
      .webrtc = &webrtc,
  };

  static const char endpoint[] = "e2e.gizclaw.com:9821";
  char token[] = "borrowed-token";
  const h2_gizclaw_e2e_config_t config = {
      .server_endpoint = {endpoint, sizeof(endpoint) - 1u},
      .registration_token = {token, sizeof(token) - 1u},
  };
  h2_gizclaw_e2e_fixture_t fixture;
  assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 1000u) ==
         H2_PAL_OK);
  assert(fixture.registration_token != token);
  assert(strcmp(fixture.registration_token, "borrowed-token") == 0);
  token[0] = 'X';
  assert(strcmp(fixture.registration_token, "borrowed-token") == 0);
  assert(strcmp(fixture.endpoint, endpoint) == 0);
  assert(strcmp(fixture.workspace_name, "h2e2e-6b6b6b6b6b6b6b6b-workspace") ==
         0);
  assert(strcmp(fixture.pet_name, "h2e2e-6b6b6b6b6b6b6b6b-pet") == 0);
  assert(fixture.runtime_profile_name[0] == '\0');
  assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 0u);
  h2_gizclaw_e2e_fixture_deinit(&fixture);
  h2_gizclaw_e2e_fixture_deinit(&fixture);
  return 0;
}
