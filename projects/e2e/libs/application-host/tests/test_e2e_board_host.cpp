#include "h2_bk7258_board.h"
#include "h2_desktop_app_support.h"
#include "h2_esp_board.h"
#include "h2_pal_e2e_application_host.h"
#include "h2_pal_e2e_host_test_sdk.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <thread>

struct Fixture {
  const char *mode;
  h2_runtime_config_t runtime{};
  h2_pal_task_api_t task{};
  h2_pal_mem_api_t alternate{};
  h2_runtime_t shadow{}, *borrowed = nullptr;
  h2_pal_task_t *service = nullptr;
  h2_pal_mutex_t *mutex = nullptr;
  std::atomic<bool> stop{false}, live{false}, entry_live{false};
  std::atomic<int> configured{0}, validated{0}, entry{0}, product_released{0};
  int prepared = 0, released = 0, starts = 0, service_starts = 0,
      service_stops = 0;
  std::thread::id owner;
};
static Fixture f;
static bool mode(const char *m) { return !std::strcmp(f.mode, m); }
static int start(void *, const h2_pal_task_options_t *options,
                 h2_pal_task_entry_t entry, void *user, h2_pal_task_t **out) {
  if (!std::strcmp(options->name, "$application/control") &&
      mode("control-start"))
    return H2_PAL_ERR_NO_MEMORY;
  if (!std::strcmp(options->name, "$application/entry") && mode("app-start"))
    return H2_PAL_ERR_NO_MEMORY;
  return h2_pal_task_start(f.runtime.task, options, entry, user, out);
}
static int join(void *, h2_pal_task_t *task) {
  return h2_pal_task_join(f.runtime.task, task);
}
static const h2_pal_task_vtable_t tasks = {start, join};
static void service(void *) {
  f.live = true;
  while (!f.stop)
    assert(h2_pal_time_sleep_ms(f.runtime.time, 1) == H2_PAL_OK);
  f.live = false;
}
static int start_service(const h2_pal_mem_api_t *allocator) {
  assert(f.service == nullptr);
  if (mode("custom"))
    assert(allocator == &f.alternate);
  h2_pal_mutex_config_t mutex{};
  mutex.name = "borrowed-command-owner";
  mutex.allocator = allocator;
  assert(h2_pal_mutex_create(f.runtime.sync, &mutex, &f.mutex) == H2_PAL_OK);
  h2_pal_task_options_t options{};
  options.name = "borrowed-command-worker";
  options.min_stack_size = 65536;
  assert(h2_pal_task_start(f.runtime.task, &options, service, nullptr,
                           &f.service) == H2_PAL_OK);
  while (!f.live)
    std::this_thread::yield();
  ++f.service_starts;
  return H2_PAL_OK;
}
static int stop_service() {
  if (!f.service)
    return H2_PAL_OK;
  if (mode("stop-fault"))
    return H2_PAL_ERR_BUSY;
  assert(!f.entry_live);
  f.stop = true;
  assert(h2_pal_task_join(f.runtime.task, f.service) == H2_PAL_OK);
  f.service = nullptr;
  assert(h2_pal_mutex_destroy(f.runtime.sync, f.mutex) == H2_PAL_OK);
  f.mutex = nullptr;
  ++f.service_stops;
  return H2_PAL_OK;
}
extern "C" {
int h2_esp_application_host_init(h2_esp_application_host_t *p,
                                 const h2_pal_application_host_api_t *api) {
  p->api = *api;
  return H2_PAL_OK;
}
int h2_bk_application_host_init(h2_bk_application_host_t *p,
                                const h2_pal_application_host_api_t *api) {
  p->api = *api;
  return H2_PAL_OK;
}
int h2_esp_board_runtime_config(h2_runtime_config_t *out) {
  assert(f.owner == std::this_thread::get_id());
  ++f.prepared;
  *out = f.runtime;
  out->task = &f.task;
  return mode("prepare") ? H2_PAL_ERR_IO : H2_PAL_OK;
}
int h2_bk7258_board_runtime_config(h2_runtime_config_t *out) {
  return h2_esp_board_runtime_config(out);
}
int h2_esp_board_runtime_deinit(void) {
  assert(f.owner == std::this_thread::get_id());
  assert(!f.live && !f.entry_live && f.service == nullptr &&
         f.mutex == nullptr);
  ++f.released;
  return mode("release-fault") ? H2_PAL_ERR_BUSY : H2_PAL_OK;
}
int h2_bk7258_board_runtime_deinit(void) {
  return h2_esp_board_runtime_deinit();
}
const h2_pal_mem_api_t *h2_esp_platform_default_allocator(void) {
  return f.runtime.mem;
}
const h2_pal_mem_api_t *h2_bk_platform_default_allocator(void) {
  return f.runtime.mem;
}
int h2_esp_h2loader_app_commands_prepare_serial_with_config(
    const h2_runtime_config_t *config,
    const h2_esp_h2loader_app_commands_config_t *commands) {
  assert(!std::strcmp(commands->active_name, "fixture") &&
         commands->hardware_capabilities == H2_LOADER_CAPABILITY_UART);
  assert(commands->h2loader_partition_id == 1 &&
         commands->coredump_partition_id == 3);
  return start_service(config->mem);
}
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
    h2_runtime_t *runtime, const char *name, uint32_t capabilities) {
  assert(!std::strcmp(name, "fixture") &&
         capabilities == H2_LOADER_CAPABILITY_UART);
  f.borrowed = runtime;
  return start_service(runtime->mem);
}
int h2_esp_h2loader_app_commands_stop_serial(void) { return stop_service(); }
int h2_bk_h2loader_stop_app_commands(void) { return stop_service(); }
int h2_esp_h2loader_app_serial_is_started(void) { return f.service != nullptr; }
int h2_bk_h2loader_app_serial_is_started(void) { return f.service != nullptr; }
}
static int quarantined_sleep(void *, uint32_t) {
  assert(!f.entry_live);
  if (mode("stop-fault")) {
    assert(f.live && f.released == 0 && f.product_released == 0);
    uint64_t alive_clock = 0;
    assert(f.borrowed && h2_pal_time_get_monotonic_ms(
                             f.borrowed->time, &alive_clock) == H2_PAL_OK);
  } else {
    if (mode("suite-error")) {
      assert(!f.live && f.released == 0 && f.product_released == 0);
      uint64_t clock = 0;
      assert(f.borrowed && h2_pal_time_get_monotonic_ms(f.borrowed->time,
                                                        &clock) == H2_PAL_OK);
      std::exit(0);
    }
    assert(mode("release-fault"));
    assert(!f.live && f.released == 1 && f.product_released == 1);
  }
  // Reaching native owner wait proves its stack-backed provider/context has
  // not returned into BSP auto-release. End this isolated test process here.
  std::exit(0);
}
extern "C" const h2_pal_time_api_t *h2_esp_platform_time_api(void) {
  static h2_pal_time_vtable_t vtable = *f.runtime.time->vtable;
  static h2_pal_time_api_t api = *f.runtime.time;
  vtable.sleep_ms = quarantined_sleep;
  api.vtable = &vtable;
  return &api;
}
extern "C" const h2_pal_time_api_t *h2_bk_platform_time_api(void) {
  return h2_esp_platform_time_api();
}
static int configure(void *user, h2_runtime_config_t *config) {
  assert(user == &f && f.owner != std::this_thread::get_id());
  ++f.configured;
  if (mode("runtime"))
    config->board = nullptr;
#if defined(H2_PAL_E2E_ESP)
  if (mode("custom")) {
    h2_runtime_config_t transport = *config;
    transport.mem = &f.alternate;
    const h2_esp_h2loader_app_commands_config_t commands = {
        "fixture", nullptr, H2_LOADER_CAPABILITY_UART, 1, 3, nullptr};
    assert(h2_esp_h2loader_app_commands_prepare_serial_with_config(
               &transport, &commands) == H2_PAL_OK);
  }
#endif
  return mode("configure") ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static int validate(void *user, h2_runtime_t *runtime) {
  assert(user == &f && f.owner != std::this_thread::get_id());
  ++f.validated;
  f.borrowed = runtime;
#if !defined(H2_PAL_E2E_ESP)
  if (mode("custom")) {
    f.shadow = *runtime;
    f.shadow.mem = &f.alternate;
    assert(h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
               &f.shadow, "fixture", H2_LOADER_CAPABILITY_UART) == H2_PAL_OK);
  }
#endif
  return mode("validate") ? H2_PAL_ERR_NOT_FOUND : H2_PAL_OK;
}
static int product_release(void *user) {
  assert(user == &f && !f.live && f.service == nullptr);
  ++f.product_released;
  return H2_PAL_OK;
}
static int entry(h2_application_host_t *host, h2_runtime_t *runtime,
                 void *user) {
  assert(user == &f && f.validated == 1);
  ++f.entry;
  if (mode("entry"))
    return H2_PAL_ERR_IO;
  if (mode("suite-error"))
    return h2_pal_e2e_application_host_finish(host, runtime, H2_PAL_ERR_IO);
  f.entry_live = true;
  int rc = h2_pal_e2e_application_host_wait(host, runtime);
  f.entry_live = false;
  return rc;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  f.mode = argv[1];
  f.owner = std::this_thread::get_id();
  f.runtime = h2::desktop::runtime_config(nullptr);
  f.alternate = *f.runtime.mem;
  f.task = {nullptr, &tasks};
  h2_application_host_config_t config{};
  config.entry = entry;
  config.user = &f;
  config.configure = configure;
  config.validate = validate;
  config.release_runtime = product_release;
  config.run_ms = 25;
  h2_application_host_result_t result{};
  int rc = h2_pal_e2e_application_host_run(
      mode("no-service") ? nullptr : "fixture", H2_LOADER_CAPABILITY_UART,
      &config, &result);
  assert(!mode("stop-fault") && !mode("release-fault") && !mode("suite-error"));
  bool pass = mode("normal") || mode("custom") || mode("no-service");
  assert((rc == H2_PAL_OK) == pass && result.retained == 0);
  assert(f.prepared == 1 && f.released == 1 && !f.live && !f.entry_live);
  assert(f.service_starts == f.service_stops);
  assert(f.entry == (pass || mode("entry") ? 1 : 0));
  assert(f.product_released ==
         (mode("prepare") || mode("control-start") ? 0 : 1));
  return 0;
}
