#include "h2_desktop_app_support.h"
#include "h2_desktop_application_host.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

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
    puts("DESKTOP_APPLICATION_HOST PASS");
}
