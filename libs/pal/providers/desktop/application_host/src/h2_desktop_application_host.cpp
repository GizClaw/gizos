#include "h2_desktop_application_host.h"

#include <condition_variable>
#include <mutex>
#ifdef __APPLE__
#include <pthread.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <new>
#include <thread>

struct h2_desktop_application_host {
    h2_pal_application_host_api_t api{};
    h2_desktop_application_host_config_t config{};
    h2_sdl3_t *window = nullptr;
    std::thread::id thread;
    h2_pal_display_api_t display{};
    std::mutex mutex;
    std::condition_variable condition;
    bool opening = false, completed = false;
    bool callbacks_open = false;
    unsigned callbacks_in_flight = 0;
    int open_result = H2_PAL_OK;
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
    return h2_pal_display_draw_bitmap(native_display(u), r, pixels, stride,
                                      format);
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

static h2_pal_result_t prepare(void *user, h2_runtime_config_t *out) {
    auto *provider = static_cast<h2_desktop_application_host_t *>(user);
    if (!on_main(provider))
        return H2_PAL_ERR_INVALID_STATE;
    h2_pal_result_t rc =
        h2_sdl3_create(&provider->config.window, &provider->window);
    if (rc != H2_PAL_OK)
        return rc;
    *out = provider->config.runtime;
    out->display = &provider->display;
    out->touch = h2_sdl3_touch(provider->window);
    return H2_PAL_OK;
}

static h2_pal_result_t activate(void *user, h2_runtime_t *) {
    auto *provider = static_cast<h2_desktop_application_host_t *>(user);
    std::lock_guard<std::mutex> lock(provider->mutex);
    provider->callbacks_open = true;
    return H2_PAL_OK;
}
static h2_pal_result_t deactivate(void *user) {
    auto *provider = static_cast<h2_desktop_application_host_t *>(user);
    std::unique_lock<std::mutex> lock(provider->mutex);
    provider->callbacks_open = false;
    provider->condition.wait(
        lock, [&] { return provider->callbacks_in_flight == 0; });
    return H2_PAL_OK;
}
static void notify(void *, h2_pal_application_host_event_t) {}

static h2_pal_result_t poll(void *user, int *out_stop) {
    auto *provider = static_cast<h2_desktop_application_host_t *>(user);
    if (!on_main(provider))
        return H2_PAL_ERR_INVALID_STATE;
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
            if (provider->callbacks_open && provider->config.event != nullptr) {
                ++provider->callbacks_in_flight;
                admitted = true;
            }
        }
        if (admitted) {
            try {
                rc = provider->config.event(provider->config.user, &event);
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
    h2_sdl3_destroy(provider->window);
    provider->window = nullptr;
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
    if (config == nullptr)
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
    if ((*out)->window != nullptr)
        return H2_PAL_ERR_BUSY;
    delete *out;
    *out = nullptr;
    return H2_PAL_OK;
}
