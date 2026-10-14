#include "h2_bleikcp.h"
#include "h2_desktop_app_support.h"
#include "h2_desktop_application_host.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#ifdef __APPLE__
#include <pthread.h>
#endif

struct LauncherContext {
  unsigned entered = 0, configured = 0, released = 0;
  bool reject = false;
};
static h2_pal_result_t launcher_configure(void *user,
                                          h2_runtime_config_t *config) {
  auto *c = static_cast<LauncherContext *>(user);
  ++c->configured;
  assert(config->crypto && config->http && config->mqtt);
  return c->reject ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static h2_pal_result_t launcher_release(void *user) {
  ++static_cast<LauncherContext *>(user)->released;
  return H2_PAL_OK;
}
static h2_pal_result_t launcher_entry(h2_application_host_t *host,
                                      h2_runtime_t *rt, void *user) {
  auto *c = static_cast<LauncherContext *>(user);
  ++c->entered;
  assert(h2_desktop_application_host_json(host));
  assert(h2_bleikcp_global_ready());
  uint8_t bytes[32]{};
  assert(h2_pal_crypto_random(rt->crypto, bytes, sizeof(bytes)) == H2_PAL_OK);
  return H2_PAL_OK;
}
static void launcher_tests() {
  static LauncherContext context;
  h2_desktop_application_host_config_t platform{};
  platform.services = H2_DESKTOP_HOST_CRYPTO | H2_DESKTOP_HOST_HTTP |
                      H2_DESKTOP_HOST_MQTT | H2_DESKTOP_HOST_JSON |
                      H2_DESKTOP_HOST_BLEIKCP;
  h2_application_host_config_t app{};
  app.entry = launcher_entry;
  app.user = &context;
  app.configure = launcher_configure;
  app.release_runtime = launcher_release;
  h2_application_host_result_t result{};
  // A configure failure must release the already opened network/JSON graph;
  // the next run then exercises the same real providers successfully.
  context.reject = true;
  assert(h2_desktop_application_host_run(&platform, &app, &result) ==
         H2_PAL_ERR_IO);
  assert(!result.retained && result.cleanup == H2_PAL_OK);
  assert(context.entered == 0 && context.released == 1);
  context.reject = false;
  for (unsigned i = 0; i < 3; ++i) {
    assert(h2_desktop_application_host_run(&platform, &app, &result) ==
           H2_PAL_OK);
    assert(!result.retained && result.cleanup == H2_PAL_OK);
  }
  assert(context.entered == 3 && context.released == 4);
  assert(!h2_bleikcp_global_ready());
  // Borrowing an existing protocol owner must leave its reference intact.
  assert(h2_bleikcp_global_init() == H2_PAL_OK);
  assert(h2_desktop_application_host_run(&platform, &app, &result) ==
         H2_PAL_OK);
  assert(h2_bleikcp_global_ready());
  assert(h2_bleikcp_global_shutdown() == H2_PAL_OK);
  const h2_pal_mem_vtable_t failed_memory = {
      [](void *, size_t) -> void * { return nullptr; },
      [](void *, void *, size_t) -> void * { return nullptr; },
      [](void *, void *) { assert(false); }};
  const h2_pal_mem_api_t failed_allocator = {nullptr, &failed_memory};
  platform.services = H2_DESKTOP_HOST_JSON;
  platform.service_allocator = &failed_allocator;
  assert(h2_desktop_application_host_run(&platform, &app, &result) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(!result.retained && result.cleanup == H2_PAL_OK);
  assert(context.entered == 4 && context.released == 5);
}

static h2_pal_result_t main_thread_entry(h2_application_host_t *host,
                                         h2_runtime_t *rt, void *) {
#ifdef __APPLE__
  assert(pthread_main_np());
#endif
  assert(h2_pal_display_open(rt->display) == H2_PAL_OK);
  // Lua can hold its job mutex while opening a display and its main-thread
  // caller can wait on that mutex. Reopening the prepared live window from
  // a worker must not wait for an owner pump in this case.
  std::thread nested(
      [&] { assert(h2_pal_display_open(rt->display) == H2_PAL_OK); });
  nested.join();
  SDL_Event close{};
  close.type = SDL_EVENT_QUIT;
  assert(SDL_PushEvent(&close));
  assert(h2_desktop_application_host_should_stop(host));
  return H2_PAL_OK;
}
static void main_thread_test() {
  h2_desktop_application_host_config_t platform{};
  platform.window = {"PAL Main Thread App", 64, 64};
  platform.entry_on_main = 1;
  h2_application_host_config_t app{};
  app.entry = main_thread_entry;
  h2_application_host_result_t result{};
  assert(h2_desktop_application_host_run(&platform, &app, &result) ==
         H2_PAL_OK);
  assert(!result.retained && result.cleanup == H2_PAL_OK);
}

static struct {
  h2_application_host_t *host;
  h2_runtime_t *runtime;
} retained;
static h2_pal_result_t retain_entry(h2_application_host_t *host,
                                    h2_runtime_t *runtime, void *) {
  retained = {host, runtime};
  return h2_application_host_retain(host, &retained, H2_PAL_ERR_BUSY);
}
static void retain_test() {
  h2_desktop_application_host_config_t platform{};
  platform.services = H2_DESKTOP_HOST_CRYPTO | H2_DESKTOP_HOST_JSON;
  h2_application_host_config_t app{};
  app.entry = retain_entry;
  h2_application_host_result_t result{};
  assert(h2_desktop_application_host_run(&platform, &app, &result) ==
         H2_PAL_ERR_BUSY);
  assert(result.retained && result.cleanup == H2_PAL_ERR_BUSY);
  assert(h2_application_host_is_retained(retained.host));
  assert(h2_desktop_application_host_json(retained.host));
  uint8_t bytes[32]{};
  assert(h2_pal_crypto_random(retained.runtime->crypto, bytes, sizeof(bytes)) ==
         H2_PAL_OK);
  // Process teardown owns this deliberately retained graph.
}

struct Context {
  std::atomic<bool> entered{false}, finished{false}, released{false};
};
static h2_pal_result_t configure(void *, h2_runtime_config_t *) {
  return H2_PAL_OK;
}
static h2_pal_result_t release_runtime(void *user) {
  auto *context = static_cast<Context *>(user);
  assert(context->finished);
  context->released = true;
  return H2_PAL_OK;
}
static h2_pal_result_t event(void *user, const h2_sdl3_event_t *event) {
  auto *context = static_cast<Context *>(user);
  if (event->kind != H2_SDL3_EVENT_CLOSE)
    return H2_PAL_OK;
  context->entered = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  // The App returns while this callback still borrows the product context.
  assert(!context->released);
  context->finished = true;
  return H2_PAL_OK;
}
static h2_pal_result_t entry(h2_application_host_t *host, h2_runtime_t *runtime,
                             void *user) {
  auto *context = static_cast<Context *>(user);
  assert(h2_pal_display_open(runtime->display) == H2_PAL_OK);
  assert(h2_pal_display_close(runtime->display) == H2_PAL_OK);
  assert(h2_pal_display_open(runtime->display) == H2_PAL_OK);
  assert(h2_application_host_ready(host) == H2_PAL_OK);
  SDL_Event close{};
  close.type = SDL_EVENT_QUIT;
  assert(SDL_PushEvent(&close));
  for (unsigned i = 0; i < 2000 && !context->entered; ++i)
    assert(h2_pal_time_sleep_ms(runtime->time, 1) == H2_PAL_OK);
  assert(context->entered);
  h2_application_host_request_stop(host);
  assert(h2_pal_display_close(runtime->display) == H2_PAL_OK);
  return H2_PAL_OK;
}
int main() {
  static Context context;
  h2_desktop_application_host_config_t config{};
  config.window = {"PAL Application Host", 64, 64};
  config.runtime = h2::desktop::runtime_config(nullptr);
  config.user = &context;
  config.event = event;
  h2_desktop_application_host_t *provider = nullptr;
  assert(h2_desktop_application_host_create(&config, &provider) == H2_PAL_OK);
  std::thread wrong_thread([&] {
    h2_desktop_application_host_t *other = nullptr;
    assert(h2_desktop_application_host_create(&config, &other) ==
           H2_PAL_ERR_INVALID_STATE);
  });
  wrong_thread.join();
  h2_application_host_config_t hc{};
  hc.allocator = config.runtime.mem;
  hc.provider = h2_desktop_application_host_api(provider);
  hc.entry = entry;
  hc.user = &context;
  hc.configure = configure;
  hc.release_runtime = release_runtime;
  hc.run_ms = 3000;
  h2_application_host_t *host = nullptr;
  assert(h2_application_host_create(&hc, &host) == H2_PAL_OK);
  h2_application_host_result_t result{};
  assert(h2_application_host_run(host, &result) == H2_PAL_OK &&
         !result.retained);
  assert(context.finished && context.released);
  assert(h2_application_host_destroy(&host) == H2_PAL_OK);
  assert(h2_desktop_application_host_destroy(&provider) == H2_PAL_OK);
  launcher_tests();
  main_thread_test();
  retain_test();
  puts("DESKTOP_APPLICATION_HOST PASS");
  std::fflush(stdout);
  std::_Exit(0);
}
