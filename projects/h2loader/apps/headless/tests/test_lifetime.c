#include "h2loader_app.h"
#include <assert.h>
#include <string.h>

static struct {
  unsigned mutexes, deinit, joins, closes, sleeps, stops;
  unsigned join_failures, close_failures;
  int serve_live, service_live;
  h2_pal_task_entry_t entry;
  void* context;
  h2_loader_t* loader;
} state;

int h2_loader_init(h2_loader_t* loader, const h2_loader_config_t* config) {
  memset(loader, 0, sizeof(*loader));
  loader->config = *config;
  state.loader = loader;
  return H2_PAL_OK;
}
void h2_loader_deinit(h2_loader_t* loader) {
  assert(loader == state.loader && !state.serve_live && !state.service_live);
  assert(state.mutexes == 0u);
  ++state.deinit;
}
int h2_loader_startup(h2_loader_t* loader, h2_loader_startup_action_t* action) {
  assert(loader == state.loader);
  *action = H2_LOADER_STARTUP_ACTION_COMMAND_MODE;
  return H2_PAL_OK;
}
int h2_loader_set_last_result(h2_loader_t* loader, int result) {
  (void)loader;
  return result;
}
int h2_loader_command_init(h2_loader_command_t* command,
                           const h2_loader_command_config_t* config) {
  memset(command, 0, sizeof(*command));
  command->config = *config;
  return H2_PAL_OK;
}
static int create_mutex(void* user, const h2_pal_mutex_config_t* config,
                        h2_pal_mutex_t** out) {
  (void)user;
  assert(config != NULL);
  *out = (h2_pal_mutex_t*)(uintptr_t)++state.mutexes;
  return H2_PAL_OK;
}
static int destroy_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  assert(mutex != NULL && state.mutexes != 0u);
  assert(!state.serve_live && !state.service_live);
  --state.mutexes;
  return H2_PAL_OK;
}
static int start(void* user, const h2_pal_task_options_t* options,
                 h2_pal_task_entry_t entry, void* context,
                 h2_pal_task_t** out) {
  (void)user;
  assert(options != NULL && state.mutexes == 2u);
  state.serve_live = 1;
  state.entry = entry;
  state.context = context;
  *out = (h2_pal_task_t*)&state;
  return H2_PAL_OK;
}
static int join(void* user, h2_pal_task_t* task) {
  (void)user;
  assert(task == (void*)&state && state.serve_live && !state.deinit);
  assert(state.mutexes == 2u);
  ++state.joins;
  if (state.join_failures != 0u) {
    --state.join_failures;
    return H2_PAL_ERR_TIMEOUT;
  }
  state.entry(state.context);
  state.serve_live = 0;
  return H2_PAL_OK;
}
static int sleep_ms(void* user, uint32_t delay) {
  (void)user;
  assert(delay == 10u && state.mutexes == 2u && !state.deinit);
  assert(state.serve_live || state.service_live);
  ++state.sleeps;
  return H2_PAL_OK;
}
static int serve(void* user, h2_loader_t* loader, h2_loader_command_t* command,
                 h2_loader_startup_action_t action) {
  (void)user;
  assert(loader == state.loader && command->config.loader == loader);
  assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
  return H2_PAL_OK;
}
static int stop(void* user) {
  (void)user;
  ++state.stops;
  return H2_PAL_OK;
}
static int open_service(void* user, h2_runtime_t* runtime,
                        const h2_loader_command_config_t* command,
                        const char* board, uint32_t capabilities, void** out) {
  (void)user;
  (void)runtime;
  (void)board;
  (void)capabilities;
  assert(command->loader == state.loader && state.serve_live);
  state.service_live = 1;
  *out = &state;
  return H2_PAL_OK;
}
static int close_service(void* user, void* service) {
  (void)user;
  assert(service == &state && state.service_live && !state.serve_live);
  assert(state.mutexes == 2u && !state.deinit);
  ++state.closes;
  if (state.close_failures != 0u) {
    --state.close_failures;
    return H2_PAL_ERR_IO;
  }
  state.service_live = 0;
  return H2_PAL_OK;
}
int main(void) {
  const h2_pal_task_vtable_t tasks = {.start = start, .join = join};
  const h2_pal_task_api_t task = {.vtable = &tasks};
  const h2_pal_sync_vtable_t syncs = {.create_mutex = create_mutex,
                                      .destroy_mutex = destroy_mutex};
  const h2_pal_sync_api_t sync = {.vtable = &syncs};
  const h2_pal_time_vtable_t times = {.sleep_ms = sleep_ms};
  const h2_pal_time_api_t time = {.vtable = &times};
  h2_runtime_t runtime = {.task = &task, .sync = &sync, .time = &time};
  const h2loader_app_config_t config = {.serve = serve, .stop_serve = stop};
  const h2loader_app_command_service_api_t service = {.open = open_service,
                                                      .close = close_service};
  for (unsigned joins = 0u; joins < 3u; ++joins) {
    for (unsigned closes = 0u; closes < 3u; ++closes) {
      memset(&state, 0, sizeof(state));
      state.join_failures = joins;
      state.close_failures = closes;
      int expected = joins != 0u    ? H2_PAL_ERR_TIMEOUT
                     : closes != 0u ? H2_PAL_ERR_IO
                                    : H2_PAL_OK;
      assert(h2loader_app_run_with_command_service(&runtime, &config,
                                                   &service) == expected);
      assert(state.deinit == 1u && state.mutexes == 0u);
      assert(state.joins == joins + 1u && state.closes == closes + 1u);
      assert(state.sleeps == joins + closes && state.stops == joins);
    }
  }
  return 0;
}
