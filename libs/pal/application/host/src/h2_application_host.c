#include "h2_application_host.h"
#include "h2_atomic.h"

#include <string.h>

struct h2_application_host {
    h2_application_host_config_t config;
    h2_pal_application_host_api_t provider;
    h2_pal_mem_api_t allocator;
    h2_runtime_t *runtime;
    h2_runtime_config_t runtime_config;
    h2_pal_task_t *control_task;
    h2_pal_task_t *app_task;
    h2_atomic_int_t stop, done, retained, cleanup, entry_active;
    h2_atomic_ptr_t retained_owner;
    /* App writes entry_result; control reads it only after a successful join.
     * application remains control-owned even when a failed join leaves the
     * App running, so its late return cannot race result publication. */
    h2_pal_result_t entry_result, application;
    const h2_pal_task_api_t *driver_task;
    const h2_pal_time_api_t *driver_time;
    int started, finished;
};

static void record_cleanup(h2_application_host_t *host, h2_pal_result_t rc) {
    int expected = H2_PAL_OK;
    (void)h2_atomic_int_compare_exchange(&host->cleanup, &expected, rc,
                                         H2_ATOMIC_ACQ_REL, H2_ATOMIC_ACQUIRE);
}

h2_pal_result_t h2_application_host_retain(h2_application_host_t *host,
                                           void *owner,
                                           h2_pal_result_t reason) {
    if (host == NULL || reason == H2_PAL_OK)
        return H2_PAL_ERR_INVALID_ARG;
    if (owner != NULL) {
        void *expected = NULL;
        if (!h2_atomic_ptr_compare_exchange(&host->retained_owner, &expected,
                                            owner, H2_ATOMIC_ACQ_REL,
                                            H2_ATOMIC_ACQUIRE) &&
            expected != owner)
            return H2_PAL_ERR_BUSY;
    }
    int expected = H2_PAL_OK;
    (void)h2_atomic_int_compare_exchange(&host->retained, &expected,
                                         reason < H2_PAL_OK ? reason
                                                            : H2_PAL_ERR_TASK,
                                         H2_ATOMIC_ACQ_REL, H2_ATOMIC_ACQUIRE);
    h2_application_host_request_stop(host);
    return H2_PAL_OK;
}

void h2_application_host_request_stop(h2_application_host_t *host) {
    if (host != NULL)
        h2_atomic_int_store(&host->stop, 1, H2_ATOMIC_RELEASE);
}

int h2_application_host_should_stop(void *user) {
    h2_application_host_t *host = user;
    return host != NULL && h2_atomic_int_load(&host->stop, H2_ATOMIC_ACQUIRE);
}

int h2_application_host_is_retained(const h2_application_host_t *host) {
    return host != NULL &&
           h2_atomic_int_load(&host->retained, H2_ATOMIC_ACQUIRE) != H2_PAL_OK;
}

h2_pal_result_t h2_application_host_ready(void *user) {
    h2_application_host_t *host = user;
    if (host == NULL ||
        !h2_atomic_int_load(&host->entry_active, H2_ATOMIC_ACQUIRE) ||
        h2_atomic_int_load(&host->retained, H2_ATOMIC_ACQUIRE))
        return H2_PAL_ERR_INVALID_STATE;
    host->provider.vtable->notify(host->provider.user,
                                  H2_PAL_APPLICATION_HOST_READY);
    return H2_PAL_OK;
}

const h2_pal_application_host_api_t *
h2_application_host_provider(const h2_application_host_t *host) {
    return host != NULL ? &host->provider : NULL;
}

static void app_entry(void *user) {
    h2_application_host_t *host = user;
    h2_atomic_int_store(&host->entry_active, 1, H2_ATOMIC_RELEASE);
    host->provider.vtable->notify(host->provider.user,
                                  H2_PAL_APPLICATION_HOST_RUNNING);
    host->entry_result =
        host->config.entry(host, host->runtime, host->config.user);
    h2_atomic_int_store(&host->entry_active, 0, H2_ATOMIC_RELEASE);
}

static void control_entry(void *user) {
    h2_application_host_t *host = user;
    int configured = host->config.configure != NULL;
    h2_pal_result_t rc =
        configured
            ? host->config.configure(host->config.user, &host->runtime_config)
            : H2_PAL_OK;
    if (rc == H2_PAL_OK)
        rc = h2_runtime_init(&host->runtime_config, &host->runtime);
    if (rc == H2_PAL_OK && host->config.validate != NULL)
        rc = host->config.validate(host->config.user, host->runtime);
    int activated = rc == H2_PAL_OK;
    if (activated)
        rc =
            host->provider.vtable->activate(host->provider.user, host->runtime);
    if (rc == H2_PAL_OK) {
        h2_pal_task_options_t options = {
            .name = "$application/entry",
            .min_stack_size =
                host->config.stack_size ? host->config.stack_size : 65536u};
        rc = h2_pal_task_start(host->runtime_config.task, &options, app_entry,
                               host, &host->app_task);
        if (rc == H2_PAL_OK) {
            rc = h2_pal_task_join(host->runtime_config.task, host->app_task);
            if (rc == H2_PAL_OK)
                host->app_task = NULL;
            else
                (void)h2_application_host_retain(host, NULL, rc);
        }
        if (rc == H2_PAL_OK)
            rc = host->entry_result;
    }
    host->application = rc;
    if (activated) {
        h2_pal_result_t cleanup =
            host->provider.vtable->deactivate(host->provider.user);
        if (cleanup != H2_PAL_OK)
            (void)h2_application_host_retain(host, NULL, cleanup);
    }
    if (h2_atomic_int_load(&host->retained, H2_ATOMIC_ACQUIRE) == H2_PAL_OK) {
        h2_pal_result_t cleanup = h2_runtime_destroy(&host->runtime);
        record_cleanup(host, cleanup);
        if (host->runtime != NULL)
            (void)h2_application_host_retain(host, NULL, cleanup);
        if (host->runtime == NULL && configured &&
            host->config.release_runtime != NULL) {
            cleanup = host->config.release_runtime(host->config.user);
            record_cleanup(host, cleanup);
            if (cleanup != H2_PAL_OK)
                (void)h2_application_host_retain(host, host->config.user,
                                                 cleanup);
        }
    }
    h2_atomic_int_store(&host->done, 1, H2_ATOMIC_RELEASE);
}

h2_pal_result_t
h2_application_host_create(const h2_application_host_config_t *config,
                           h2_application_host_t **out_host) {
    if (out_host == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out_host = NULL;
    if (config == NULL || config->allocator == NULL ||
        config->provider == NULL || config->entry == NULL ||
        (config->configure == NULL && config->release_runtime != NULL))
        return H2_PAL_ERR_INVALID_ARG;
    if (config->allocator->vtable == NULL ||
        config->allocator->vtable->alloc == NULL ||
        config->allocator->vtable->free == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    const h2_pal_application_host_vtable_t *v = config->provider->vtable;
    if (v == NULL || !v->prepare || !v->activate || !v->poll ||
        !v->deactivate || !v->release || !v->notify)
        return H2_PAL_ERR_INVALID_ARG;
    h2_application_host_t *host =
        h2_pal_mem_alloc(config->allocator, sizeof(*host));
    if (host == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    memset(host, 0, sizeof(*host));
    host->config = *config;
    host->allocator = *config->allocator;
    host->provider = *config->provider;
    host->application = H2_PAL_ERR_TASK;
    host->entry_result = H2_PAL_ERR_TASK;
    h2_atomic_result_t ar = h2_atomic_int_init(&host->stop, 0);
    if (ar == H2_ATOMIC_OK)
        ar = h2_atomic_int_init(&host->done, 0);
    if (ar == H2_ATOMIC_OK)
        ar = h2_atomic_int_init(&host->retained, 0);
    if (ar == H2_ATOMIC_OK)
        ar = h2_atomic_int_init(&host->cleanup, 0);
    if (ar == H2_ATOMIC_OK)
        ar = h2_atomic_int_init(&host->entry_active, 0);
    if (ar == H2_ATOMIC_OK)
        ar = h2_atomic_ptr_init(&host->retained_owner, NULL);
    if (ar != H2_ATOMIC_OK) {
        h2_atomic_int_destroy(&host->stop);
        h2_atomic_int_destroy(&host->done);
        h2_atomic_int_destroy(&host->retained);
        h2_atomic_int_destroy(&host->cleanup);
        h2_atomic_int_destroy(&host->entry_active);
        h2_atomic_ptr_destroy(&host->retained_owner);
        h2_pal_mem_free(config->allocator, host);
        return ar == H2_ATOMIC_UNSUPPORTED ? H2_PAL_ERR_UNSUPPORTED
                                           : H2_PAL_ERR_NO_MEMORY;
    }
    *out_host = host;
    return H2_PAL_OK;
}

h2_pal_result_t
h2_application_host_run(h2_application_host_t *host,
                        h2_application_host_result_t *out_result) {
    if (host == NULL || out_result == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out_result = (h2_application_host_result_t){0};
    if (host->started) {
        out_result->cleanup = H2_PAL_ERR_INVALID_STATE;
        out_result->retained =
            !host->finished || h2_application_host_is_retained(host);
        return H2_PAL_ERR_INVALID_STATE;
    }
    host->started = 1;
    const h2_pal_application_host_api_t *p = &host->provider;
    h2_pal_result_t rc = p->vtable->prepare(p->user, &host->runtime_config);
    host->driver_task = host->runtime_config.task;
    host->driver_time = host->runtime_config.time;
    uint64_t start_ms = 0;
    if (rc == H2_PAL_OK && host->config.run_ms)
        rc = h2_pal_time_get_monotonic_ms(host->driver_time, &start_ms);
    if (rc == H2_PAL_OK) {
        const h2_pal_task_options_t options = {
            .name = "$application/control",
            .min_stack_size = host->config.control_stack_size
                                  ? host->config.control_stack_size
                                  : 65536u};
        rc = h2_pal_task_start(host->driver_task, &options, control_entry, host,
                               &host->control_task);
    }
    if (rc == H2_PAL_OK) {
        int notified = 0;
        while (!h2_atomic_int_load(&host->done, H2_ATOMIC_ACQUIRE)) {
            int stop = 0;
            h2_pal_result_t polled = p->vtable->poll(p->user, &stop);
            if (polled != H2_PAL_OK) {
                record_cleanup(host, polled);
                h2_application_host_request_stop(host);
            }
            uint64_t now_ms = 0;
            if (host->config.run_ms &&
                h2_pal_time_get_monotonic_ms(host->driver_time, &now_ms) !=
                    H2_PAL_OK) {
                record_cleanup(host, H2_PAL_ERR_IO);
                stop = 1;
            }
            if (stop || (host->config.run_ms &&
                         now_ms - start_ms >= host->config.run_ms))
                h2_application_host_request_stop(host);
            if (!notified && h2_application_host_should_stop(host)) {
                notified = 1;
                p->vtable->notify(p->user, H2_PAL_APPLICATION_HOST_STOPPING);
            }
            h2_pal_result_t slept = h2_pal_time_sleep_ms(host->driver_time, 1u);
            if (slept != H2_PAL_OK) {
                record_cleanup(host, slept);
                h2_application_host_request_stop(host);
            }
        }
        rc = h2_pal_task_join(host->driver_task, host->control_task);
        if (rc == H2_PAL_OK)
            host->control_task = NULL;
        else
            (void)h2_application_host_retain(host, NULL, rc);
    } else
        host->application = rc;
    int retained = h2_atomic_int_load(&host->retained, H2_ATOMIC_ACQUIRE);
    if (retained == H2_PAL_OK) {
        rc = p->vtable->release(p->user);
        record_cleanup(host, rc);
        if (rc != H2_PAL_OK) {
            (void)h2_application_host_retain(host, p->user, rc);
            retained = rc;
        }
    }
    host->finished = 1;
    *out_result = (h2_application_host_result_t){
        host->application,
        retained != H2_PAL_OK
            ? retained
            : h2_atomic_int_load(&host->cleanup, H2_ATOMIC_ACQUIRE),
        retained != H2_PAL_OK};
    return out_result->cleanup != H2_PAL_OK ? out_result->cleanup
                                            : host->application;
}

h2_pal_result_t h2_application_host_destroy(h2_application_host_t **pointer) {
    if (pointer == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    h2_application_host_t *host = *pointer;
    if (host == NULL)
        return H2_PAL_OK;
    if ((host->started && !host->finished) ||
        h2_atomic_int_load(&host->retained, H2_ATOMIC_ACQUIRE))
        return H2_PAL_ERR_BUSY;
    h2_pal_mem_api_t allocator = host->allocator;
    h2_atomic_int_destroy(&host->stop);
    h2_atomic_int_destroy(&host->done);
    h2_atomic_int_destroy(&host->retained);
    h2_atomic_int_destroy(&host->cleanup);
    h2_atomic_int_destroy(&host->entry_active);
    h2_atomic_ptr_destroy(&host->retained_owner);
    h2_pal_mem_free(&allocator, host);
    *pointer = NULL;
    return H2_PAL_OK;
}
