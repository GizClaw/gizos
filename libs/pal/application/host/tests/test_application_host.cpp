#include "h2_application_host.h"
#include "h2_desktop_app_support.h"
#include "h2_runtime_test.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>

struct Fixture {
    const char *mode;
    h2_runtime_config_t runtime{};
    h2_pal_task_api_t task{};
    h2_pal_task_t *control = nullptr, *app = nullptr;
    std::atomic<int> starts{0}, prepared{0}, configured{0}, validated{0};
    std::atomic<int> activated{0}, deactivated{0}, released{0},
        product_released{0};
    std::atomic<int> workers{0}, ready{0}, stopping{0}, polls{0};
    h2_runtime_t *borrowed = nullptr;
    h2_runtime_test_control_t *test_control = nullptr;
};
static bool mode(Fixture *f, const char *value) {
    return !std::strcmp(f->mode, value);
}
static int start(void *user, const h2_pal_task_options_t *options,
                 h2_pal_task_entry_t entry, void *ctx, h2_pal_task_t **out) {
    auto *f = static_cast<Fixture *>(user);
    const int number = ++f->starts;
    if ((number == 1 && mode(f, "control-start")) ||
        (number == 2 && mode(f, "app-start")))
        return H2_PAL_ERR_NO_MEMORY;
    int rc = h2_pal_task_start(f->runtime.task, options, entry, ctx, out);
    if (rc == H2_PAL_OK)
        (number == 1 ? f->control : f->app) = *out;
    return rc;
}
static int join(void *user, h2_pal_task_t *task) {
    auto *f = static_cast<Fixture *>(user);
    if (task == f->app && mode(f, "app-join"))
        while (f->ready == 0)
            h2_pal_time_sleep_ms(f->runtime.time, 1);
    if ((task == f->app && mode(f, "app-join")) ||
        (task == f->control && mode(f, "control-join")))
        return H2_PAL_ERR_TIMEOUT;
    return h2_pal_task_join(f->runtime.task, task);
}
static const h2_pal_task_vtable_t tasks = {start, join};
static h2_pal_result_t prepare(void *user, h2_runtime_config_t *out) {
    auto *f = static_cast<Fixture *>(user);
    ++f->prepared;
    *out = f->runtime;
    out->task = &f->task;
    return mode(f, "prepare") ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
    auto *f = static_cast<Fixture *>(user);
    ++f->configured;
    if (mode(f, "runtime"))
        config->board = nullptr;
    return mode(f, "configure") ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static h2_pal_result_t validate(void *user, h2_runtime_t *) {
    auto *f = static_cast<Fixture *>(user);
    ++f->validated;
    return mode(f, "validate") ? H2_PAL_ERR_NOT_FOUND : H2_PAL_OK;
}
static h2_pal_result_t activate(void *user, h2_runtime_t *) {
    auto *f = static_cast<Fixture *>(user);
    ++f->activated;
    return mode(f, "activate") ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static h2_pal_result_t deactivate(void *user) {
    auto *f = static_cast<Fixture *>(user);
    ++f->deactivated;
    return mode(f, "deactivate") ? H2_PAL_ERR_BUSY : H2_PAL_OK;
}
static h2_pal_result_t product_release(void *user) {
    auto *f = static_cast<Fixture *>(user);
    ++f->product_released;
    return mode(f, "product-release") ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static h2_pal_result_t release(void *user) {
    auto *f = static_cast<Fixture *>(user);
    ++f->released;
    assert(f->workers == 0);
    return mode(f, "release") ? H2_PAL_ERR_BUSY : H2_PAL_OK;
}
static h2_pal_result_t poll(void *user, int *out_stop) {
    auto *f = static_cast<Fixture *>(user);
    *out_stop = mode(f, "external-stop") && ++f->polls > 5;
    return mode(f, "poll") ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static void notify(void *user, h2_pal_application_host_event_t event) {
    auto *f = static_cast<Fixture *>(user);
    if (event == H2_PAL_APPLICATION_HOST_READY)
        ++f->ready;
    if (event == H2_PAL_APPLICATION_HOST_STOPPING)
        ++f->stopping;
}
static const h2_pal_application_host_vtable_t vtable = {
    prepare, activate, poll, deactivate, release, notify};
static void worker(void *user) {
    auto *f = static_cast<Fixture *>(user);
    h2_pal_time_sleep_ms(f->runtime.time, 30);
    --f->workers;
}
static h2_pal_result_t entry(h2_application_host_t *host, h2_runtime_t *runtime,
                             void *user) {
    auto *f = static_cast<Fixture *>(user);
    f->borrowed = runtime;
    assert(h2_application_host_ready(host) == H2_PAL_OK);
    if (mode(f, "runtime-busy"))
        assert(h2_runtime_test_control_open(runtime, &f->test_control) ==
               H2_PAL_OK);
    if (mode(f, "retain")) {
        assert(h2_application_host_retain(host, f, H2_PAL_ERR_BUSY) ==
               H2_PAL_OK);
        assert(h2_application_host_retain(host, f, H2_PAL_ERR_IO) == H2_PAL_OK);
        return H2_PAL_ERR_BUSY;
    }
    if (mode(f, "entry"))
        return H2_PAL_ERR_IO;
    if (mode(f, "workers")) {
        ++f->workers;
        h2_pal_task_t *task = nullptr;
        assert(h2_pal_task_start(f->runtime.task, nullptr, worker, f, &task) ==
               H2_PAL_OK);
        assert(h2_pal_task_join(f->runtime.task, task) == H2_PAL_OK);
    }
    while (!h2_application_host_should_stop(host))
        assert(h2_pal_time_sleep_ms(runtime->time, 1) == H2_PAL_OK);
    return H2_PAL_OK;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    // Persistent contexts deliberately survive any retained graph.
    static Fixture f;
    f.mode = argv[1];
    f.runtime = h2::desktop::runtime_config(nullptr);
    f.task = {&f, &tasks};
    const h2_pal_application_host_api_t provider = {&f, &vtable};
    h2_application_host_config_t config{};
    config.allocator = f.runtime.mem;
    config.provider = &provider;
    config.entry = entry;
    config.user = &f;
    config.configure = configure;
    config.validate = validate;
    config.release_runtime = product_release;
    config.run_ms = 20;
    h2_application_host_t *host = nullptr;
    assert(h2_application_host_create(&config, &host) == H2_PAL_OK);
    assert(h2_application_host_ready(host) == H2_PAL_ERR_INVALID_STATE);
    h2_application_host_result_t result{};
    const int rc = h2_application_host_run(host, &result);
    assert(h2_application_host_ready(host) == H2_PAL_ERR_INVALID_STATE);
    const bool retained = mode(&f, "app-join") || mode(&f, "control-join") ||
                          mode(&f, "deactivate") || mode(&f, "runtime-busy") ||
                          mode(&f, "retain") || mode(&f, "product-release") ||
                          mode(&f, "release");
    assert(result.retained == retained);
    const bool pass =
        mode(&f, "success") || mode(&f, "workers") || mode(&f, "external-stop");
    assert((rc == H2_PAL_OK) == pass);
    h2_application_host_result_t repeated{};
    assert(h2_application_host_run(host, &repeated) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(repeated.retained == retained);
    if (retained) {
        assert(h2_application_host_destroy(&host) == H2_PAL_ERR_BUSY &&
               host != nullptr);
        assert(f.released == (mode(&f, "release") ? 1 : 0));
        if (mode(&f, "runtime-busy"))
            assert(f.borrowed->board != nullptr && f.test_control != nullptr);
    } else {
        assert(f.released == 1);
        assert(h2_application_host_destroy(&host) == H2_PAL_OK &&
               host == nullptr);
    }
    if (mode(&f, "configure") || mode(&f, "runtime") || mode(&f, "validate") ||
        mode(&f, "activate"))
        assert(f.product_released == 1);
    if (mode(&f, "activate"))
        assert(f.deactivated == 1);
    puts("APPLICATION_HOST_FAULT_TEST PASS");
}
