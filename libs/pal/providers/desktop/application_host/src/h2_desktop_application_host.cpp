#include "h2_desktop_application_host.h"
#include "h2_bleikcp.h"
#include "h2_desktop_app_support.h"
#include "h2_lvgl_platform.h"
#include "h2_yyjson_json.h"
#include "lvgl.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#ifdef __APPLE__
#include <pthread.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <memory>
#include <new>
#include <thread>
#include <type_traits>

struct h2_desktop_application_host {
  h2_pal_application_host_api_t api{};
  h2_desktop_application_host_config_t config{};
  h2_sdl3_t *window = nullptr;
  h2::desktop::OwnedDisplay display_owner;
  std::thread::id thread;
  h2_pal_display_api_t display{};
  h2_runtime_component_mapper_t mapper{};
  std::mutex mutex;
  std::condition_variable condition;
  bool opening = false, completed = false;
  bool callbacks_open = false;
  unsigned callbacks_in_flight = 0;
  int open_result = H2_PAL_OK;
  bool prepared = false;
  h2_application_host_config_t app{};
  h2::desktop::OwnedNetworkServices network;
  std::unique_ptr<h2::desktop::OwnedFilesystem> filesystem;
  std::unique_ptr<h2::desktop::OwnedPreference> preference;
  std::unique_ptr<h2::desktop::OwnedAudio> audio;
  h2_corehttp_t *http = nullptr;
  h2_pal_http_api_t http_api{};
  h2_yyjson_json_t *json = nullptr;
  std::unique_ptr<h2::desktop::OwnedLvgl> lvgl;
  bool lvgl_platform = false;
  h2_application_host_t *pending_host = nullptr;
  h2_runtime_t *pending_runtime = nullptr;
  bool entry_pending = false, entry_done = false;
  h2_pal_result_t entry_result = H2_PAL_OK, poll_result = H2_PAL_OK;
  uint64_t started_ms = 0u;
  std::unique_ptr<h2::desktop::OwnedStatusSurface> status;
  std::atomic<int> desired_status{
      static_cast<int>(h2::desktop::ProcessState::kRunning)};
  int rendered_status = 0;
  bool bleikcp = false;
};

static bool process_main() {
#ifdef __APPLE__
  return pthread_main_np() != 0;
#else
  return syscall(SYS_gettid) == getpid();
#endif
}

static bool on_main(h2_desktop_application_host_t *provider) {
  return provider->thread == std::this_thread::get_id() && process_main();
}

// SDL window creation/recreation must occur on the process main thread.
// Other SDL PAL operations copy state under its own locks and defer rendering.
static int display_open(void *user) {
  auto *p = static_cast<h2_desktop_application_host_t *>(user);
  if (on_main(p))
    return h2_pal_display_open(h2_sdl3_display(p->window));
  // Opening an already live display is idempotent. A Lua worker can hold
  // its job mutex while opening it, so never wait for the main thread when
  // no native window operation is needed.
  h2_display_info_t info{};
  if (h2_pal_display_get_info(h2_sdl3_display(p->window), &info) == H2_PAL_OK)
    return H2_PAL_OK;
  std::unique_lock<std::mutex> lock(p->mutex);
  p->condition.wait(lock, [&] { return !p->opening; });
  p->opening = true;
  p->completed = false;
  p->condition.wait(lock, [&] { return p->completed; });
  int rc = p->open_result;
  p->opening = false;
  p->condition.notify_all();
  return rc;
}
static const h2_pal_display_api_t *native_display(void *user) {
  return h2_sdl3_display(
      static_cast<h2_desktop_application_host_t *>(user)->window);
}
static int display_info(void *u, h2_display_info_t *info) {
  return h2_pal_display_get_info(native_display(u), info);
}
static int display_draw(void *u, const h2_display_rect_t *r, const void *pixels,
                        size_t stride, h2_display_pixel_format_t format) {
  // C accepts unknown integer formats. Validate their representation before
  // loading a C++ enum, just as the native SDL provider does.
  std::underlying_type_t<h2_display_pixel_format_t> code;
  std::memcpy(&code, &format, sizeof(code));
  if (code != H2_DISPLAY_PIXEL_RGB565 && code != H2_DISPLAY_PIXEL_RGB888 &&
      code != H2_DISPLAY_PIXEL_RGB444) {
    if (r == nullptr || pixels == nullptr)
      return H2_DISPLAY_ERR_INVALID_ARG;
    h2_display_info_t info{};
    const int rc = h2_pal_display_get_info(native_display(u), &info);
    return rc == H2_PAL_OK ? H2_DISPLAY_ERR_UNSUPPORTED : rc;
  }
  return h2_pal_display_draw_bitmap(native_display(u), r, pixels, stride,
                                    static_cast<h2_display_pixel_format_t>(code));
}
static int display_present(void *u) {
  return h2_pal_display_present(native_display(u));
}
static int display_brightness(void *u, uint32_t value) {
  return h2_pal_display_set_brightness_percent(native_display(u), value);
}
static int display_close(void *u) {
  return h2_pal_display_close(native_display(u));
}
static const h2_pal_display_vtable_t display_vtable = {
    display_open,    display_info,       display_draw,
    display_present, display_brightness, display_close};

static h2_pal_result_t mapping_list(void *user, h2_runtime_component_t filter,
                                    h2_runtime_component_mapping_cb_t callback,
                                    void *callback_user) {
  if (!callback)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_NONE &&
      filter != H2_RUNTIME_COMPONENT_BUTTON)
    return H2_PAL_OK;
  auto &config = static_cast<h2_desktop_application_host_t *>(user)->config;
  for (size_t i = 0; i < config.mapping_count; ++i) {
    h2_pal_result_t rc = callback(callback_user, &config.mappings[i]);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t mapping_get(void *user,
                                   h2_runtime_component_id_t component,
                                   h2_pal_periph_id_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  auto &config = static_cast<h2_desktop_application_host_t *>(user)->config;
  for (size_t i = 0; i < config.mapping_count; ++i) {
    if (config.mappings[i].component_id == component) {
      *out = config.mappings[i].periph_id;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}
static const h2_runtime_component_mapper_vtable_t mapping_vtable = {
    mapping_list, mapping_get};

static h2_pal_result_t open_status(h2_desktop_application_host_t *provider) {
  if (provider->status)
    return H2_PAL_OK;
  provider->status.reset(new (std::nothrow) h2::desktop::OwnedStatusSurface());
  if (!provider->status)
    return H2_PAL_ERR_NO_MEMORY;
  return static_cast<h2_pal_result_t>(h2::desktop::open_status_surface(
      &provider->display_owner, provider->config.status_name,
      provider->status.get()));
}
static h2_pal_result_t render_status(h2_desktop_application_host_t *provider) {
  const int desired = provider->desired_status.load();
  if (!provider->status || desired == provider->rendered_status)
    return H2_PAL_OK;
  int rc = h2::desktop::set_process_state(
      provider->status.get(), static_cast<h2::desktop::ProcessState>(desired));
  if (rc == H2_PAL_OK)
    provider->rendered_status = desired;
  return static_cast<h2_pal_result_t>(rc);
}

static h2_pal_result_t prepare(void *user, h2_runtime_config_t *out) {
  auto *provider = static_cast<h2_desktop_application_host_t *>(user);
  if (!on_main(provider))
    return H2_PAL_ERR_INVALID_STATE;
  const auto &c = provider->config;
  provider->prepared = true;
  try {
    const h2::desktop::Layout layout = {
        nullptr,         c.window.title,     c.window.width,
        c.window.height, c.mounts,           c.mount_count,
        c.peripherals,   c.peripheral_count, nullptr};
    int rc = H2_PAL_OK;
    if (c.peripherals != nullptr || c.peripheral_count != 0u)
      rc = h2::desktop::configure_layout(layout);
    if (rc == H2_PAL_OK && c.mount_count != 0u) {
      provider->filesystem = std::make_unique<h2::desktop::OwnedFilesystem>();
      rc = h2::desktop::open_filesystem(layout, provider->filesystem.get());
    }
    *out = c.runtime.mem != nullptr ? c.runtime
                                    : h2::desktop::runtime_config(nullptr);
    if (provider->filesystem)
      out->fs = provider->filesystem->api();
    if (c.mapping_count != 0u)
      out->component_mapper = &provider->mapper;
    if (rc == H2_PAL_OK && (c.services & H2_DESKTOP_HOST_PREFERENCE)) {
      provider->preference = std::make_unique<h2::desktop::OwnedPreference>();
      if (c.preference_path != nullptr) {
        const h2_sqlite_config_t config = {c.preference_path};
        rc = h2_sqlite_create(&config, &provider->preference->handle);
      } else {
        rc = h2::desktop::open_preference(provider->preference.get());
      }
      out->pref = provider->preference->api();
    }
    const h2_pal_mem_api_t *mem = c.service_allocator != nullptr
                                      ? c.service_allocator
                                      : h2_desktop_platform_default_allocator();
    if (rc == H2_PAL_OK &&
        (c.services & (H2_DESKTOP_HOST_CRYPTO | H2_DESKTOP_HOST_HTTP |
                       H2_DESKTOP_HOST_MQTT | H2_DESKTOP_HOST_WEBRTC))) {
      rc = h2::desktop::open_network_services(
          c.services & H2_DESKTOP_HOST_MQTT,
          c.services & H2_DESKTOP_HOST_WEBRTC, mem,
          c.mqtt_records != 0u ? c.mqtt_records : 8u, &provider->network);
      out->crypto = provider->network.crypto();
      out->net = h2::desktop::host_net_api();
      if (c.services & H2_DESKTOP_HOST_MQTT)
        out->mqtt = provider->network.mqtt();
      if (c.services & H2_DESKTOP_HOST_WEBRTC)
        out->webrtc = provider->network.webrtc();
    }
    if (rc == H2_PAL_OK && (c.services & H2_DESKTOP_HOST_HTTP)) {
      h2_corehttp_config_t config = c.http;
      if (config.allocator == nullptr)
        config.allocator = mem;
      if (config.net == nullptr)
        config.net = out->net;
      if (config.time == nullptr)
        config.time = out->time;
      rc = h2_corehttp_create(&config, &provider->http, &provider->http_api);
      out->http = &provider->http_api;
    }
    if (rc == H2_PAL_OK && (c.services & H2_DESKTOP_HOST_JSON))
      rc = h2_yyjson_json_create(mem, &provider->json);
    if (rc == H2_PAL_OK && (c.services & H2_DESKTOP_HOST_BLEIKCP)) {
      rc = h2_bleikcp_global_init();
      provider->bleikcp = rc == H2_PAL_OK;
    }
    if (rc == H2_PAL_OK && (c.services & H2_DESKTOP_HOST_AUDIO)) {
      provider->audio = std::make_unique<h2::desktop::OwnedAudio>();
      rc = h2::desktop::open_audio(c.require_real_audio, provider->audio.get());
      if (rc == H2_PAL_OK &&
          (c.audio_capture.on_mic || c.audio_capture.on_speaker))
        rc = h2_portaudio_set_capture_hooks(provider->audio->handle,
                                            &c.audio_capture);
      out->audio = provider->audio->api();
    }
    if (rc == H2_PAL_OK && (c.services & H2_DESKTOP_HOST_LVGL)) {
      const h2_lvgl_platform_config_t config = {
          out->mem, out->task, out->sync, out->queue, out->time, 0u, 0u};
      rc = h2_lvgl_platform_init(&config);
      if (rc == H2_PAL_OK) {
        provider->lvgl_platform = true;
        lv_init();
      }
    }
    if (rc == H2_PAL_OK && c.window.width != 0 && c.window.height != 0) {
      rc = h2_sdl3_create(&c.window, &provider->window);
      provider->display_owner.handle = provider->window;
      if (rc == H2_PAL_OK && c.entry_on_main)
        rc = h2_pal_display_open(h2_sdl3_display(provider->window));
      if (rc == H2_PAL_OK && c.render_capture != nullptr)
        rc = h2_sdl3_set_render_capture(provider->window, c.render_capture,
                                        c.user);
      if (rc == H2_PAL_OK && c.frame_capture != nullptr)
        rc = h2_sdl3_set_frame_capture(provider->window, c.frame_capture,
                                       c.user);
      out->display = &provider->display;
      out->touch = h2_sdl3_touch(provider->window);
      if (rc == H2_PAL_OK && provider->lvgl_platform) {
        provider->lvgl = std::make_unique<h2::desktop::OwnedLvgl>();
        rc = h2::desktop::open_lvgl(&provider->display_owner,
                                    provider->lvgl.get());
      }
      if (rc == H2_PAL_OK && c.status_name && !c.status_on_error)
        rc = open_status(provider);
    }
    if (rc != H2_PAL_OK)
      return static_cast<h2_pal_result_t>(rc);
    return H2_PAL_OK;
  } catch (const std::bad_alloc &) {
    return H2_PAL_ERR_NO_MEMORY;
  }
}

static h2_pal_result_t activate(void *user, h2_runtime_t *runtime) {
  auto *provider = static_cast<h2_desktop_application_host_t *>(user);
  if (provider->config.input) {
    h2_pal_result_t rc = h2_runtime_input_start(runtime, nullptr);
    if (rc != H2_PAL_OK)
      return rc;
  }
  std::lock_guard<std::mutex> lock(provider->mutex);
  provider->callbacks_open = true;
  return H2_PAL_OK;
}
static h2_pal_result_t deactivate(void *user) {
  auto *provider = static_cast<h2_desktop_application_host_t *>(user);
  std::unique_lock<std::mutex> lock(provider->mutex);
  provider->callbacks_open = false;
  provider->condition.wait(lock,
                           [&] { return provider->callbacks_in_flight == 0; });
  return H2_PAL_OK;
}
static void notify(void *user, h2_pal_application_host_event_t event) {
  auto *provider = static_cast<h2_desktop_application_host_t *>(user);
  provider->desired_status.store(
      static_cast<int>(event == H2_PAL_APPLICATION_HOST_STOPPING
                           ? h2::desktop::ProcessState::kStopping
                           : h2::desktop::ProcessState::kRunning));
}

static h2_pal_result_t poll(void *user, int *out_stop) {
  auto *provider = static_cast<h2_desktop_application_host_t *>(user);
  if (!on_main(provider))
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_result_t status_rc = render_status(provider);
  if (status_rc != H2_PAL_OK)
    return status_rc;
  h2_application_host_t *pending_host = nullptr;
  h2_runtime_t *pending_runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(provider->mutex);
    if (provider->entry_pending) {
      provider->entry_pending = false;
      pending_host = provider->pending_host;
      pending_runtime = provider->pending_runtime;
    }
  }
  if (pending_host != nullptr) {
    h2_pal_result_t result;
    try {
      result = provider->app.entry(pending_host, pending_runtime,
                                   provider->app.user);
    } catch (...) {
      result = H2_PAL_ERR_IO;
      (void)h2_application_host_retain(pending_host, provider->app.user,
                                       result);
    }
    if (result != H2_PAL_OK && provider->config.status_name &&
        !h2_application_host_is_retained(pending_host)) {
      h2_pal_result_t opened = open_status(provider);
      if (opened == H2_PAL_OK) {
        provider->desired_status.store(
            static_cast<int>(h2::desktop::ProcessState::kFailed));
        (void)h2_desktop_application_host_wait(pending_host);
      }
    }
    std::lock_guard<std::mutex> lock(provider->mutex);
    provider->entry_result = result;
    provider->entry_done = true;
    provider->condition.notify_all();
  }
  if (provider->poll_result != H2_PAL_OK)
    return provider->poll_result;
  {
    std::lock_guard<std::mutex> lock(provider->mutex);
    if (provider->opening && !provider->completed) {
      provider->open_result =
          h2_pal_display_open(h2_sdl3_display(provider->window));
      provider->completed = true;
      provider->condition.notify_all();
    }
  }
  *out_stop = 0;
  if (provider->window == nullptr)
    return H2_PAL_OK;
  h2_sdl3_event_t event{};
  h2_pal_result_t rc;
  for (unsigned count = 0; count < 64; ++count) {
    rc = h2_sdl3_poll_event(provider->window, &event);
    if (rc != H2_PAL_OK)
      break;
    if (event.kind == H2_SDL3_EVENT_CLOSE)
      *out_stop = 1;
    bool admitted = false;
    {
      std::lock_guard<std::mutex> lock(provider->mutex);
      if (provider->callbacks_open) {
        ++provider->callbacks_in_flight;
        admitted = true;
      }
    }
    if (admitted) {
      try {
        h2::desktop::handle_lvgl_event(provider->lvgl.get(), event);
        if (event.kind == H2_SDL3_EVENT_FOCUS_LOST)
          h2_desktop_platform_handle_focus_lost();
        if (event.kind == H2_SDL3_EVENT_KEY && event.key >= H2_SDL3_KEY_SPACE &&
            event.key <= H2_SDL3_KEY_DIGIT_9)
          (void)h2_desktop_platform_handle_key(
              static_cast<h2_desktop_key_t>(event.key), event.pressed,
              event.repeat);
        rc = provider->config.event
                 ? provider->config.event(provider->config.user, &event)
                 : H2_PAL_OK;
      } catch (...) {
        rc = H2_PAL_ERR_IO;
      }
      {
        std::lock_guard<std::mutex> lock(provider->mutex);
        --provider->callbacks_in_flight;
        provider->condition.notify_all();
      }
      if (rc != H2_PAL_OK)
        return rc;
    }
  }
  return rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_OK ? H2_PAL_OK : rc;
}

static h2_pal_result_t release(void *user) {
  auto *provider = static_cast<h2_desktop_application_host_t *>(user);
  if (!on_main(provider))
    return H2_PAL_ERR_INVALID_STATE;
  {
    std::lock_guard<std::mutex> lock(provider->mutex);
    if (provider->callbacks_open || provider->callbacks_in_flight != 0 ||
        provider->opening)
      return H2_PAL_ERR_BUSY;
  }
  h2_pal_result_t rc = h2_yyjson_json_destroy(&provider->json);
  if (rc != H2_PAL_OK)
    return rc;
  if (provider->audio != nullptr) {
    if (provider->audio->handle != nullptr) {
      rc = h2_portaudio_set_capture_hooks(provider->audio->handle, nullptr);
      if (rc != H2_PAL_OK)
        return rc;
    }
    provider->audio.reset();
  }
  h2_corehttp_destroy(provider->http);
  provider->http = nullptr;
  if (provider->bleikcp) {
    rc = static_cast<h2_pal_result_t>(h2_bleikcp_global_shutdown());
    if (rc != H2_PAL_OK)
      return rc;
    provider->bleikcp = false;
  }
  rc = static_cast<h2_pal_result_t>(provider->network.reset());
  if (rc != H2_PAL_OK)
    return rc;
  provider->preference.reset();
  provider->filesystem.reset();
  provider->status.reset();
  provider->lvgl.reset();
  if (provider->lvgl_platform) {
    lv_deinit();
    h2_lvgl_platform_deinit();
    provider->lvgl_platform = false;
  }
  if (provider->window) {
    rc = h2_sdl3_set_render_capture(provider->window, nullptr, nullptr);
    if (rc != H2_PAL_OK)
      return rc;
    rc = h2_sdl3_set_frame_capture(provider->window, nullptr, nullptr);
    if (rc != H2_PAL_OK)
      return rc;
  }
  h2_sdl3_destroy(provider->window);
  provider->window = nullptr;
  provider->display_owner.handle = nullptr;
  provider->prepared = false;
  return H2_PAL_OK;
}

static const h2_pal_application_host_vtable_t vtable = {
    prepare, activate, poll, deactivate, release, notify};

h2_pal_result_t h2_desktop_application_host_create(
    const h2_desktop_application_host_config_t *config,
    h2_desktop_application_host_t **out) {
  if (out == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  *out = nullptr;
  if (config == nullptr || (config->mount_count && !config->mounts) ||
      (config->peripheral_count && !config->peripherals) ||
      (config->mapping_count && !config->mappings) ||
      (config->window.width == 0) != (config->window.height == 0) ||
      (config->services & H2_DESKTOP_HOST_LVGL && config->window.width == 0))
    return H2_PAL_ERR_INVALID_ARG;
  if (config->status_name &&
      ((config->services & H2_DESKTOP_HOST_LVGL) || config->window.width == 0 ||
       !config->entry_on_main))
    return H2_PAL_ERR_INVALID_ARG;
  if (!process_main())
    return H2_PAL_ERR_INVALID_STATE;
  auto *provider = new (std::nothrow) h2_desktop_application_host_t;
  if (provider == nullptr)
    return H2_PAL_ERR_NO_MEMORY;
  provider->config = *config;
  provider->thread = std::this_thread::get_id();
  provider->api = {provider, &vtable};
  provider->display = {provider, &display_vtable};
  provider->mapper = {provider, &mapping_vtable};
  *out = provider;
  return H2_PAL_OK;
}

const h2_pal_application_host_api_t *
h2_desktop_application_host_api(h2_desktop_application_host_t *provider) {
  return provider != nullptr ? &provider->api : nullptr;
}

h2_pal_result_t
h2_desktop_application_host_destroy(h2_desktop_application_host_t **out) {
  if (out == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  if (*out == nullptr)
    return H2_PAL_OK;
  if (!on_main(*out))
    return H2_PAL_ERR_INVALID_STATE;
  if ((*out)->prepared)
    return H2_PAL_ERR_BUSY;
  delete *out;
  *out = nullptr;
  return H2_PAL_OK;
}

static h2_pal_result_t app_configure(void *user, h2_runtime_config_t *config) {
  auto &app = static_cast<h2_desktop_application_host_t *>(user)->app;
  return app.configure ? app.configure(app.user, config) : H2_PAL_OK;
}
static h2_pal_result_t app_validate(void *user, h2_runtime_t *runtime) {
  auto &app = static_cast<h2_desktop_application_host_t *>(user)->app;
  return app.validate ? app.validate(app.user, runtime) : H2_PAL_OK;
}
static h2_pal_result_t app_release(void *user) {
  auto &app = static_cast<h2_desktop_application_host_t *>(user)->app;
  return app.release_runtime ? app.release_runtime(app.user) : H2_PAL_OK;
}
static h2_pal_result_t app_before(void *user) {
  auto &app = static_cast<h2_desktop_application_host_t *>(user)->app;
  return app.before_prepare ? app.before_prepare(app.user) : H2_PAL_OK;
}
static h2_pal_result_t app_entry(h2_application_host_t *host,
                                 h2_runtime_t *runtime, void *user) {
  auto *provider = static_cast<h2_desktop_application_host_t *>(user);
  auto &app = provider->app;
  if (provider->config.entry_on_main) {
    std::unique_lock<std::mutex> lock(provider->mutex);
    provider->pending_host = host;
    provider->pending_runtime = runtime;
    provider->entry_pending = true;
    provider->condition.wait(lock, [&] { return provider->entry_done; });
    return provider->entry_result;
  }
  return app.entry(host, runtime, app.user);
}

h2_pal_result_t h2_desktop_application_host_run(
    const h2_desktop_application_host_config_t *config,
    const h2_application_host_config_t *app,
    h2_application_host_result_t *result) {
  if (result == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  *result = {};
  if (app == nullptr || app->entry == nullptr || app->provider != nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  if (config != nullptr && (config->services & H2_DESKTOP_HOST_LVGL) &&
      !config->entry_on_main)
    return H2_PAL_ERR_INVALID_ARG;
  h2_desktop_application_host_t *provider = nullptr;
  h2_pal_result_t rc = h2_desktop_application_host_create(config, &provider);
  if (rc != H2_PAL_OK)
    return rc;
  provider->app = *app;
  if (provider->app.run_ms == 0u) {
    const char *duration = std::getenv("H2_DESKTOP_RUN_MS");
    if (duration != nullptr && *duration != '\0') {
      char *end = nullptr;
      unsigned long value = std::strtoul(duration, &end, 10);
      if (*end != '\0' || value == 0u || value > UINT32_MAX) {
        (void)h2_desktop_application_host_destroy(&provider);
        return H2_PAL_ERR_INVALID_ARG;
      }
      provider->app.run_ms = static_cast<uint32_t>(value);
    }
  }
  h2_application_host_config_t hc = *app;
  hc.run_ms = provider->app.run_ms;
  hc.allocator =
      app->allocator ? app->allocator : h2_desktop_platform_default_allocator();
  hc.provider = &provider->api;
  hc.entry = app_entry;
  hc.user = provider;
  hc.configure = app_configure;
  hc.validate = app_validate;
  hc.release_runtime = app_release;
  hc.before_prepare = app_before;
  h2_application_host_t *host = nullptr;
  (void)h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(),
                                     &provider->started_ms);
  rc = h2_application_host_create(&hc, &host);
  if (rc == H2_PAL_OK)
    rc = h2_application_host_run(host, result);
  else
    result->application = rc;
  if (result->retained) {
    std::printf(
        "H2_DESKTOP_APP result=FAIL application=%d cleanup=%d retained=1\n",
        result->application, result->cleanup);
    return rc;
  }
  h2_pal_result_t cleanup = h2_application_host_destroy(&host);
  if (cleanup == H2_PAL_OK)
    cleanup = h2_desktop_application_host_destroy(&provider);
  if (cleanup != H2_PAL_OK) {
    result->cleanup = cleanup;
    result->retained = 1;
    return cleanup;
  }
  std::printf("H2_DESKTOP_APP result=%s application=%d cleanup=%d retained=0\n",
              rc == H2_PAL_OK ? "PASS" : "FAIL", result->application,
              result->cleanup);
  return rc;
}

const h2_pal_json_api_t *
h2_desktop_application_host_json(h2_application_host_t *host) {
  const auto *api = h2_application_host_provider(host);
  auto *provider =
      api ? static_cast<h2_desktop_application_host_t *>(api->user) : nullptr;
  return provider ? h2_yyjson_json_api(provider->json) : nullptr;
}

static h2::desktop::OwnedLvgl *owned_lvgl(h2_application_host_t *host) {
  const auto *api = h2_application_host_provider(host);
  auto *provider =
      api ? static_cast<h2_desktop_application_host_t *>(api->user) : nullptr;
  return provider ? provider->lvgl.get() : nullptr;
}
void *h2_desktop_application_host_lvgl_display(h2_application_host_t *host) {
  auto *lvgl = owned_lvgl(host);
  return lvgl ? lvgl->display() : nullptr;
}
void *h2_desktop_application_host_lvgl_keyboard(h2_application_host_t *host) {
  auto *lvgl = owned_lvgl(host);
  return lvgl ? lvgl->keyboard() : nullptr;
}
void *h2_desktop_application_host_lvgl_wheel(h2_application_host_t *host) {
  auto *lvgl = owned_lvgl(host);
  return lvgl ? lvgl->wheel() : nullptr;
}

int h2_desktop_application_host_should_stop(void *user) {
  auto *host = static_cast<h2_application_host_t *>(user);
  const auto *api = h2_application_host_provider(host);
  auto *provider =
      api ? static_cast<h2_desktop_application_host_t *>(api->user) : nullptr;
  if (provider == nullptr || !on_main(provider))
    return 1;
  uint64_t now = 0;
  if (provider->app.run_ms != 0u &&
      h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(), &now) ==
          H2_PAL_OK &&
      now - provider->started_ms >= provider->app.run_ms)
    h2_application_host_request_stop(host);
  int stopped = 0;
  h2_pal_result_t rc = poll(provider, &stopped);
  if (rc != H2_PAL_OK)
    provider->poll_result = rc;
  if (stopped || rc != H2_PAL_OK)
    h2_application_host_request_stop(host);
  return h2_application_host_should_stop(host);
}
h2_pal_result_t h2_desktop_application_host_wait(h2_application_host_t *host) {
  while (!h2_desktop_application_host_should_stop(host)) {
    h2_pal_result_t rc =
        h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 16u);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}
h2_pal_result_t
h2_desktop_application_host_pointer(h2_application_host_t *host,
                                    h2_sdl3_pointer_state_t *state) {
  const auto *api = h2_application_host_provider(host);
  auto *provider =
      api ? static_cast<h2_desktop_application_host_t *>(api->user) : nullptr;
  if (provider == nullptr)
    return H2_PAL_ERR_INVALID_STATE;
  return h2_sdl3_read_pointer(provider->window, state);
}
h2_sdl3_t *h2_desktop_application_host_sdl(h2_application_host_t *host) {
  const auto *api = h2_application_host_provider(host);
  auto *provider =
      api ? static_cast<h2_desktop_application_host_t *>(api->user) : nullptr;
  return provider ? provider->window : nullptr;
}
h2_portaudio_t *
h2_desktop_application_host_portaudio(h2_application_host_t *host) {
  const auto *api = h2_application_host_provider(host);
  auto *provider =
      api ? static_cast<h2_desktop_application_host_t *>(api->user) : nullptr;
  return provider && provider->audio ? provider->audio->handle : nullptr;
}
