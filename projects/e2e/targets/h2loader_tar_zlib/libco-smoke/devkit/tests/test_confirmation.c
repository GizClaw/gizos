#include "confirmation.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static h2_runtime_t s_runtime;
static int s_mutex;
static h2_pal_sync_vtable_t s_vtable;
static h2_pal_sync_api_t s_sync;
static struct {
  int get_result, lock_result, confirm_result, unlock_result;
  int owned;
  int missing_mutex;
  char order[8];
  size_t count;
} s_state;

static void record(char step) {
  assert(s_state.count + 1u < sizeof(s_state.order));
  s_state.order[s_state.count++] = step;
}

static int lock(void *user, h2_pal_mutex_t *mutex) {
  assert(user == &s_state && mutex == (h2_pal_mutex_t *)&s_mutex);
  assert(!s_state.owned);
  record('L');
  if (s_state.lock_result == H2_PAL_OK) s_state.owned = 1;
  return s_state.lock_result;
}

static int unlock(void *user, h2_pal_mutex_t *mutex) {
  assert(user == &s_state && mutex == (h2_pal_mutex_t *)&s_mutex);
  assert(s_state.owned);
  record('U');
  s_state.owned = 0;
  return s_state.unlock_result;
}

static int get_config(h2_loader_app_client_config_t *config) {
  record('G');
  *config = (h2_loader_app_client_config_t){
      .operation_sync = &s_sync,
      .operation_mutex = s_state.missing_mutex
                             ? NULL : (h2_pal_mutex_t *)&s_mutex,
  };
  return s_state.get_result;
}

static int confirm(h2_runtime_t *runtime) {
  assert(runtime == &s_runtime && s_state.owned);
  record('C');
  return s_state.confirm_result;
}

static void reset(void) {
  memset(&s_state, 0, sizeof(s_state));
  s_vtable = (h2_pal_sync_vtable_t){.lock_mutex = lock,
                                     .unlock_mutex = unlock};
  s_sync = (h2_pal_sync_api_t){.user = &s_state, .vtable = &s_vtable};
}

int main(void) {
  static const struct {
    int get, lock, confirm, unlock, expected;
    const char *order;
  } cases[] = {
      {0, 0, 0, 0, 0, "GLCU"},
      {-101, 0, 0, 0, -101, "G"},
      {0, -102, 0, 0, -102, "GL"},
      {0, 0, -103, 0, -103, "GLCU"},
      {0, 0, 0, -104, -104, "GLCU"},
      {0, 0, -103, -104, -103, "GLCU"},
  };
  for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    reset();
    s_state.get_result = cases[i].get;
    s_state.lock_result = cases[i].lock;
    s_state.confirm_result = cases[i].confirm;
    s_state.unlock_result = cases[i].unlock;
    assert(h2_libco_smoke_confirm(&s_runtime, get_config, confirm) ==
           cases[i].expected);
    assert(strcmp(s_state.order, cases[i].order) == 0 && !s_state.owned);
  }
  reset();
  s_state.missing_mutex = 1;
  assert(h2_libco_smoke_confirm(&s_runtime, get_config, confirm) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(strcmp(s_state.order, "G") == 0);
  reset();
  s_vtable.unlock_mutex = NULL;
  assert(h2_libco_smoke_confirm(&s_runtime, get_config, confirm) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(strcmp(s_state.order, "G") == 0 && !s_state.owned);
  reset();
  assert(h2_libco_smoke_confirm(NULL, get_config, confirm) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_libco_smoke_confirm(&s_runtime, NULL, confirm) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_libco_smoke_confirm(&s_runtime, get_config, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(s_state.count == 0u);
  puts("Libco confirmation lock and error paths passed");
  return 0;
}
