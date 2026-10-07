#include "h2_iperf_server_app_internal.h"

#include <stdio.h>
#include <string.h>

static bool valid_mode(h2_iperf_server_app_mode_t mode) {
  return mode == H2_IPERF_SERVER_APP_MODE_IPV4 ||
         mode == H2_IPERF_SERVER_APP_MODE_IPV6 ||
         mode == H2_IPERF_SERVER_APP_MODE_DUAL;
}
static int lock(h2_iperf_server_app_t *app) {
  return h2_pal_mutex_lock(app->runtime->sync, app->mutex);
}
static void unlock(h2_iperf_server_app_t *app) {
  (void)h2_pal_mutex_unlock(app->runtime->sync, app->mutex);
}
static bool stopped(void *user) {
  h2_iperf_server_app_worker_t *worker = user;
  return h2_atomic_bool_load(&worker->app->stop, H2_ATOMIC_ACQUIRE);
}
static void progress(void *user, const h2_iperf_progress_t *value) {
  h2_iperf_server_app_worker_t *worker = user;
  h2_iperf_server_app_t *app = worker->app;
  if (lock(app) != H2_PAL_OK) {
    h2_atomic_bool_store(&app->stop, true, H2_ATOMIC_RELEASE);
    return;
  }
  app->snapshot.streams[worker->index].active = true;
  app->snapshot.streams[worker->index].progress = *value;
  unlock(app);
}
static void publish_phase(h2_iperf_server_app_t *app,
                          h2_iperf_server_app_phase_t phase, int error) {
  if (lock(app) != H2_PAL_OK)
    return;
  app->snapshot.phase = phase;
  app->snapshot.error = error;
  unlock(app);
  char message[100];
  (void)snprintf(message, sizeof(message),
                 "H2_IPERF_SERVER_STATE phase=%d rc=%d", (int)phase, error);
  (void)h2_pal_log_write(app->runtime->log, H2_PAL_LOG_INFO, "iperf-server",
                         message);
}
static void serve(void *user) {
  h2_iperf_server_app_worker_t *worker = user;
  h2_iperf_server_app_t *app = worker->app;
  while (!stopped(worker)) {
    h2_iperf_result_t result = {0};
    int rc = h2_iperf_server_run_once(worker->server, 100u, &result);
    if (rc == H2_PAL_ERR_TIMEOUT && result.cookie[0] == '\0')
      continue;
    if (lock(app) == H2_PAL_OK) {
      h2_iperf_server_app_stream_t *stream =
          &app->snapshot.streams[worker->index];
      stream->active = false;
      stream->result_code = rc;
      if (rc == H2_PAL_OK) {
        stream->result = result;
        ++stream->completed_tests;
      }
      unlock(app);
    }
    if (!stopped(worker)) {
      char message[200];
      (void)snprintf(message, sizeof(message),
                     "H2_IPERF_SERVER_TEST family=%u rc=%d proto=%d reverse=%d "
                     "bytes=%llu ms=%u",
                     worker->index == 0 ? 4u : 6u, rc, (int)result.protocol,
                     result.reverse, (unsigned long long)result.local.bytes,
                     (unsigned)result.local.duration_ms);
      (void)h2_pal_log_write(app->runtime->log, H2_PAL_LOG_INFO, "iperf-server",
                             message);
      (void)h2_pal_time_sleep_ms(app->runtime->time, 10u);
    }
  }
}
static int start_worker(h2_iperf_server_app_t *app, unsigned index,
                        const h2_pal_net_addr_t *address) {
  h2_iperf_server_app_worker_t *worker = &app->workers[index];
  worker->app = app;
  worker->index = index;
  const h2_pal_net_family_t family =
      index == 0 ? H2_PAL_NET_FAMILY_IPV4 : H2_PAL_NET_FAMILY_IPV6;
  if (address->family != family)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_iperf_config_t config = {
      .mem = app->runtime->mem,
      .net = app->runtime->net,
      .time = app->runtime->time,
      .crypto = app->runtime->crypto,
      .log = app->runtime->log,
      .should_stop = stopped,
      .on_progress = progress,
      .callback_user = worker,
      .max_json_len = 4096u,
  };
  const h2_pal_net_bind_t bind = {.type = H2_PAL_NET_BIND_SOURCE_ADDR,
                                  .source_addr = *address};
  const h2_iperf_server_params_t params = {
      .family = family,
      .bind = &bind,
      .port = app->snapshot.port,
      .control_timeout_ms = 5000u,
      .max_block_len = 128u * 1024u,
  };
  int rc = h2_iperf_server_create(&config, &params, &worker->server);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_task_options_t options = {
      .name = index == 0 ? "iperf-server/ipv4" : "iperf-server/ipv6",
      .min_stack_size = 8192u,
  };
  return h2_pal_task_start(app->runtime->task, &options, serve, worker,
                           &worker->task);
}
static int stop_service(h2_iperf_server_app_t *app) {
  h2_atomic_bool_store(&app->stop, true, H2_ATOMIC_RELEASE);
  for (unsigned i = 0; i < 2; ++i) {
    h2_iperf_server_app_worker_t *worker = &app->workers[i];
    if (worker->task != NULL) {
      int rc = h2_pal_task_join(app->runtime->task, worker->task);
      if (rc != H2_PAL_OK)
        return rc;
      worker->task = NULL;
    }
    h2_iperf_server_destroy(&worker->server);
  }
  if (app->network_active) {
    int rc = app->config.network_stop(app->config.network_user);
    if (rc != H2_PAL_OK)
      return rc;
    app->network_active = false;
  }
  if (lock(app) == H2_PAL_OK) {
    memset(&app->snapshot.network, 0, sizeof(app->snapshot.network));
    for (unsigned i = 0; i < 2; ++i)
      app->snapshot.streams[i].active = false;
    unlock(app);
  }
  return H2_PAL_OK;
}
static void manage(void *user) {
  h2_iperf_server_app_t *app = user;
  bool serving = false;
  bool cleanup_pending = false;
  for (;;) {
    if (h2_atomic_bool_load(&app->shutdown, H2_ATOMIC_ACQUIRE)) {
      int cleanup = stop_service(app);
      publish_phase(app, cleanup == H2_PAL_OK ? H2_IPERF_SERVER_APP_STOPPED
                                             : H2_IPERF_SERVER_APP_STOPPING,
                    cleanup);
      break;
    }
    int state_error = lock(app);
    if (state_error != H2_PAL_OK) {
      /* Retain failed cleanup for destroy, and reject new requests after the
       * manager exits. Never spin forever on a failed mutex or network hook. */
      h2_atomic_bool_store(&app->shutdown, true, H2_ATOMIC_RELEASE);
      int cleanup = stop_service(app);
      publish_phase(app, cleanup == H2_PAL_OK ? H2_IPERF_SERVER_APP_ERROR
                                             : H2_IPERF_SERVER_APP_STOPPING,
                    cleanup == H2_PAL_OK ? state_error : cleanup);
      break;
    }
    bool requested = app->requested;
    bool cleanup_requested = app->cleanup_requested;
    app->cleanup_requested = false;
    h2_iperf_server_app_mode_t mode = app->snapshot.mode;
    if (requested && !serving && !cleanup_pending) {
      app->snapshot.phase = H2_IPERF_SERVER_APP_STARTING;
      memset(app->snapshot.streams, 0, sizeof(app->snapshot.streams));
      h2_atomic_bool_store(&app->stop, false, H2_ATOMIC_RELEASE);
    }
    unlock(app);
    if (requested && !serving && !cleanup_pending) {
      publish_phase(app, H2_IPERF_SERVER_APP_STARTING, H2_PAL_OK);
      h2_iperf_server_app_network_t network = {0};
      /* A failed start can retain partial ownership. Stop must run before
       * releasing its borrowed network state, even without a ready address. */
      app->network_active = true;
      int rc =
          app->config.network_start(app->config.network_user, mode, &network);
      if (rc == H2_PAL_OK) {
        if (lock(app) == H2_PAL_OK) {
          app->snapshot.network = network;
          unlock(app);
        }
        if (mode != H2_IPERF_SERVER_APP_MODE_IPV6)
          rc = start_worker(app, 0, &network.ipv4);
        if (rc == H2_PAL_OK && mode != H2_IPERF_SERVER_APP_MODE_IPV4)
          rc = start_worker(app, 1, &network.ipv6);
      }
      if (rc == H2_PAL_OK &&
          !h2_atomic_bool_load(&app->stop, H2_ATOMIC_ACQUIRE)) {
        serving = true;
        publish_phase(app, H2_IPERF_SERVER_APP_LISTENING, H2_PAL_OK);
      } else {
        int cleanup = stop_service(app);
        cleanup_pending = cleanup != H2_PAL_OK;
        if (lock(app) == H2_PAL_OK) {
          app->requested = false;
          unlock(app);
        }
        publish_phase(app,
                      cleanup_pending ? H2_IPERF_SERVER_APP_STOPPING
                      : rc == H2_PAL_OK ? H2_IPERF_SERVER_APP_STOPPED
                                       : H2_IPERF_SERVER_APP_ERROR,
                      cleanup_pending ? cleanup : rc);
      }
    } else if (!requested && cleanup_requested) {
      publish_phase(app, H2_IPERF_SERVER_APP_STOPPING, H2_PAL_OK);
      int rc = stop_service(app);
      serving = false;
      cleanup_pending = rc != H2_PAL_OK;
      if (rc == H2_PAL_OK) {
        publish_phase(app, H2_IPERF_SERVER_APP_STOPPED, H2_PAL_OK);
      } else {
        publish_phase(app, H2_IPERF_SERVER_APP_STOPPING, rc);
      }
    } else if (!requested && !serving && !cleanup_pending) {
      if (lock(app) == H2_PAL_OK) {
        if (app->snapshot.phase == H2_IPERF_SERVER_APP_STARTING)
          app->snapshot.phase = H2_IPERF_SERVER_APP_STOPPED;
        unlock(app);
      }
    }
    (void)h2_pal_time_sleep_ms(app->runtime->time, 20u);
  }
}

h2_pal_result_t
h2_iperf_server_app_create(h2_runtime_t *runtime,
                           const h2_iperf_server_app_config_t *config,
                           h2_iperf_server_app_t **out_app) {
  if (out_app == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_app = NULL;
  if (runtime == NULL || runtime->mem == NULL || runtime->net == NULL ||
      runtime->time == NULL || runtime->task == NULL || runtime->sync == NULL ||
      config == NULL || config->network_start == NULL ||
      config->network_stop == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_iperf_server_app_t *app = h2_pal_mem_alloc(runtime->mem, sizeof(*app));
  if (app == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(app, 0, sizeof(*app));
  app->runtime = runtime;
  app->config = *config;
  app->snapshot.mode = H2_IPERF_SERVER_APP_MODE_DUAL;
  app->snapshot.port = config->port == 0 ? H2_IPERF_DEFAULT_PORT : config->port;
  int rc = H2_PAL_ERR_NO_MEMORY;
  if (h2_atomic_bool_init(&app->stop, true) != H2_ATOMIC_OK)
    goto failed;
  if (h2_atomic_bool_init(&app->shutdown, false) != H2_ATOMIC_OK)
    goto failed;
  const h2_pal_mutex_config_t mutex = {.name = "iperf-server/state",
                                       .allocator = runtime->mem};
  rc = h2_pal_mutex_create(runtime->sync, &mutex, &app->mutex);
  if (rc != H2_PAL_OK)
    goto failed;
  const h2_pal_task_options_t options = {.name = "iperf-server/control",
                                         .min_stack_size = 8192u};
  rc = h2_pal_task_start(runtime->task, &options, manage, app, &app->manager);
  if (rc != H2_PAL_OK)
    goto failed;
  *out_app = app;
  return H2_PAL_OK;
failed:
  if (app->mutex != NULL)
    (void)h2_pal_mutex_destroy(runtime->sync, app->mutex);
  h2_atomic_bool_destroy(&app->shutdown);
  h2_atomic_bool_destroy(&app->stop);
  h2_pal_mem_free(runtime->mem, app);
  return rc;
}
h2_pal_result_t h2_iperf_server_app_request(h2_iperf_server_app_t *app,
                                            h2_iperf_server_app_mode_t mode,
                                            bool running) {
  if (app == NULL || !valid_mode(mode))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = lock(app);
  if (rc != H2_PAL_OK)
    return rc;
  if (h2_atomic_bool_load(&app->shutdown, H2_ATOMIC_ACQUIRE)) {
    unlock(app);
    return H2_PAL_ERR_CLOSED;
  }
  if (running && app->snapshot.phase != H2_IPERF_SERVER_APP_STOPPED &&
      app->snapshot.phase != H2_IPERF_SERVER_APP_ERROR) {
    unlock(app);
    return H2_PAL_ERR_BUSY;
  }
  if (!running && mode != app->snapshot.mode &&
      app->snapshot.phase != H2_IPERF_SERVER_APP_STOPPED &&
      app->snapshot.phase != H2_IPERF_SERVER_APP_ERROR) {
    unlock(app);
    return H2_PAL_ERR_BUSY;
  }
  app->snapshot.mode = mode;
  app->requested = running;
  app->cleanup_requested = !running;
  if (running) {
    app->snapshot.phase = H2_IPERF_SERVER_APP_STARTING;
    app->snapshot.error = 0;
  } else {
    app->snapshot.error = 0;
    h2_atomic_bool_store(&app->stop, true, H2_ATOMIC_RELEASE);
  }
  unlock(app);
  return H2_PAL_OK;
}
h2_pal_result_t
h2_iperf_server_app_snapshot(h2_iperf_server_app_t *app,
                             h2_iperf_server_app_snapshot_t *out_snapshot) {
  if (out_snapshot == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_snapshot, 0, sizeof(*out_snapshot));
  if (app == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = lock(app);
  if (rc != H2_PAL_OK)
    return rc;
  *out_snapshot = app->snapshot;
  unlock(app);
  return H2_PAL_OK;
}
h2_pal_result_t h2_iperf_server_app_destroy(h2_iperf_server_app_t **out_app) {
  if (out_app == NULL || *out_app == NULL)
    return H2_PAL_OK;
  h2_iperf_server_app_t *app = *out_app;
  h2_atomic_bool_store(&app->shutdown, true, H2_ATOMIC_RELEASE);
  h2_atomic_bool_store(&app->stop, true, H2_ATOMIC_RELEASE);
  int rc;
  if (app->manager != NULL) {
    rc = h2_pal_task_join(app->runtime->task, app->manager);
    if (rc != H2_PAL_OK)
      return rc;
    app->manager = NULL;
  }
  /* The manager may have exited with unjoined tasks or an active network.
   * Retry once on the caller, keeping all state intact if cleanup still fails. */
  rc = stop_service(app);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_pal_mutex_destroy(app->runtime->sync, app->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  h2_atomic_bool_destroy(&app->stop);
  h2_atomic_bool_destroy(&app->shutdown);
  h2_pal_mem_free(app->runtime->mem, app);
  *out_app = NULL;
  return H2_PAL_OK;
}
