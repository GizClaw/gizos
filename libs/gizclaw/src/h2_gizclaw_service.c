#include "h2_gizclaw_service_internal.h"

#include "h2_gizclaw_task_names.h"

#include <string.h>

#ifdef H2_GIZCLAW_TESTING
static const h2_gizclaw_service_client_ops_t *s_client_ops;

void h2_gizclaw_service_test_set_client_ops(
    const h2_gizclaw_service_client_ops_t *ops) {
  s_client_ops = ops;
}
#endif

static h2_pal_result_t client_init(const h2_gizclaw_config_t *config,
                                   h2_gizclaw_client_t **out_client) {
#ifdef H2_GIZCLAW_TESTING
  if (s_client_ops != NULL && s_client_ops->init != NULL)
    return s_client_ops->init(config, out_client);
#endif
  return (h2_pal_result_t)h2_gizclaw_client_init(config, out_client);
}

static h2_pal_result_t client_connect(h2_gizclaw_client_t *client) {
#ifdef H2_GIZCLAW_TESTING
  if (s_client_ops != NULL && s_client_ops->connect != NULL)
    return s_client_ops->connect(client);
#endif
  return (h2_pal_result_t)h2_gizclaw_client_connect(client);
}

static h2_pal_result_t client_poll(h2_gizclaw_client_t *client,
                                   int timeout_ms) {
#ifdef H2_GIZCLAW_TESTING
  if (s_client_ops != NULL && s_client_ops->poll != NULL)
    return s_client_ops->poll(client, timeout_ms);
#endif
  return (h2_pal_result_t)h2_gizclaw_client_poll(client, timeout_ms);
}

static h2_pal_result_t
client_set_event_handler(h2_gizclaw_client_t *client,
                         h2_gizclaw_client_event_fn event, void *event_user) {
#ifdef H2_GIZCLAW_TESTING
  if (s_client_ops != NULL) {
    return s_client_ops->set_event_handler != NULL
               ? s_client_ops->set_event_handler(client, event, event_user)
               : H2_PAL_OK;
  }
#endif
  return (h2_pal_result_t)h2_gizclaw_client_set_event_handler(client, event,
                                                              event_user);
}

static h2_pal_result_t client_dispatch_event(h2_gizclaw_client_t *client) {
#ifdef H2_GIZCLAW_TESTING
  if (s_client_ops != NULL) {
    return s_client_ops->dispatch_event != NULL
               ? s_client_ops->dispatch_event(client)
               : H2_PAL_ERR_WOULD_BLOCK;
  }
#endif
  return (h2_pal_result_t)h2_gizclaw_client_dispatch_event(client, 0, NULL,
                                                           NULL);
}

static h2_pal_result_t client_close(h2_gizclaw_client_t *client) {
#ifdef H2_GIZCLAW_TESTING
  if (s_client_ops != NULL && s_client_ops->close != NULL)
    return s_client_ops->close(client);
#endif
  return (h2_pal_result_t)h2_gizclaw_client_close(client);
}

static void client_deinit(h2_gizclaw_client_t *client) {
#ifdef H2_GIZCLAW_TESTING
  if (s_client_ops != NULL && s_client_ops->deinit != NULL) {
    s_client_ops->deinit(client);
    return;
  }
#endif
  h2_gizclaw_client_deinit(client);
}

static h2_pal_result_t lock_service(h2_gizclaw_service_t *service) {
  return h2_pal_mutex_lock(service->config.sync, service->mutex);
}

static void unlock_service(h2_gizclaw_service_t *service) {
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
}

static bool service_cancel_requested(void *user) {
  h2_gizclaw_service_t *service = user;
  h2_gizclaw_cancel_fn original = NULL;
  void *original_user = NULL;
  bool canceled = true;
  if (service != NULL && lock_service(service) == H2_PAL_OK) {
    canceled = service->stopping ||
               (service->current != NULL && service->current->cancel_requested);
    original = service->original_cancel;
    original_user = service->original_cancel_user;
    unlock_service(service);
  }
  return canceled || (original != NULL && original(original_user));
}

bool h2_gizclaw_cancel_requested(
    const h2_gizclaw_cancel_token_t *cancel_token) {
  if (cancel_token == NULL || cancel_token->operation == NULL)
    return true;
  h2_gizclaw_operation_t *operation = cancel_token->operation;
  h2_gizclaw_service_t *service = operation->service;
  bool canceled = true;
  h2_gizclaw_cancel_fn original = NULL;
  void *original_user = NULL;
  if (service != NULL && lock_service(service) == H2_PAL_OK) {
    canceled = service->stopping || operation->cancel_requested;
    original = service->original_cancel;
    original_user = service->original_cancel_user;
    unlock_service(service);
  }
  return canceled || (original != NULL && original(original_user));
}

static void free_operation_if_unreferenced(h2_gizclaw_operation_t *operation) {
  if (!operation->caller_reference && !operation->internal_reference) {
    h2_pal_mem_free(operation->service->config.client_config->allocator,
                    operation);
  }
}

static h2_pal_result_t enqueue_completion_locked(
    h2_gizclaw_service_t *service, h2_gizclaw_operation_t *operation,
    h2_gizclaw_operation_terminal_kind_t kind, h2_pal_result_t result) {
  operation->result.terminal_kind = kind;
  operation->result.result = result;
  operation->state = H2_GIZCLAW_OPERATION_COMPLETION_PENDING;
  if (operation->completion_queued)
    return H2_PAL_OK;
  const int rc =
      h2_pal_queue_send(service->config.queue, service->completion_queue,
                        &operation, H2_PAL_QUEUE_NO_WAIT);
  if (rc == H2_PAL_OK)
    operation->completion_queued = true;
  return (h2_pal_result_t)rc;
}

h2_pal_result_t h2_gizclaw_operation_dispatch_call(
    const h2_gizclaw_cancel_token_t *cancel_token,
    h2_gizclaw_operation_dispatch_fn callback, void *user) {
  if (cancel_token == NULL || cancel_token->operation == NULL ||
      callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_operation_t *operation = cancel_token->operation;
  h2_gizclaw_service_t *service = operation->service;
  h2_pal_result_t rc = lock_service(service);
  if (rc != H2_PAL_OK)
    return rc;
  if (service->stopping || operation->cancel_requested) {
    unlock_service(service);
    return H2_PAL_ERR_CLOSED;
  }
  operation->progress = callback;
  operation->progress_user = user;
  operation->progress_pending = true;
  operation->progress_claimed = false;
  operation->state = H2_GIZCLAW_OPERATION_PROGRESS_PENDING;
  rc = (h2_pal_result_t)h2_pal_queue_send(service->config.queue,
                                          service->completion_queue, &operation,
                                          H2_PAL_QUEUE_NO_WAIT);
  if (rc == H2_PAL_OK)
    operation->completion_queued = true;
  while (rc == H2_PAL_OK && operation->progress_pending &&
         (operation->progress_claimed ||
          (!service->stopping && !operation->cancel_requested))) {
    rc = h2_pal_cond_wait(service->config.sync, service->progress_cond,
                          service->mutex, H2_PAL_SYNC_WAIT_FOREVER);
  }
  if (rc == H2_PAL_OK && (service->stopping || operation->cancel_requested))
    rc = H2_PAL_ERR_CLOSED;
  if (rc == H2_PAL_OK)
    rc = operation->progress_result;
  operation->progress_pending = false;
  operation->progress_claimed = false;
  operation->progress = NULL;
  operation->progress_user = NULL;
  unlock_service(service);
  return rc;
}

static void close_client(h2_gizclaw_service_t *service) {
  if (service->client == NULL)
    return;
  if (service->config.cleanup != NULL)
    service->config.cleanup(service->config.cleanup_user, service->client);
  (void)client_close(service->client);
  client_deinit(service->client);
  service->client = NULL;
}

static void complete_queued_as_closed(h2_gizclaw_service_t *service) {
  h2_gizclaw_operation_t *operation = NULL;
  while (h2_pal_queue_recv(service->config.queue, service->request_queue,
                           &operation, H2_PAL_QUEUE_NO_WAIT) == H2_PAL_OK) {
    if (lock_service(service) != H2_PAL_OK)
      return;
    (void)enqueue_completion_locked(service, operation,
                                    H2_GIZCLAW_OPERATION_SERVICE_CLOSED,
                                    H2_PAL_ERR_CLOSED);
    unlock_service(service);
  }
}

static void mark_terminal(h2_gizclaw_service_t *service,
                          h2_pal_result_t result) {
  if (lock_service(service) != H2_PAL_OK)
    return;
  if (!service->stopping) {
    service->stopping = true;
    service->terminal_pending = true;
    service->terminal_result = result;
  }
  unlock_service(service);
  complete_queued_as_closed(service);
}

static void service_worker(void *ctx) {
  h2_gizclaw_service_t *service = ctx;
  h2_pal_result_t rc = H2_PAL_OK;
  if (service->config.prepare != NULL) {
    rc = service->config.prepare(service->config.prepare_user,
                                 service_cancel_requested, service);
  }
  if (rc == H2_PAL_OK)
    rc = client_init(&service->client_config, &service->client);
  if (rc == H2_PAL_OK)
    rc = client_connect(service->client);
  if (rc == H2_PAL_OK && service->config.on_event != NULL) {
    rc = client_set_event_handler(service->client, service->config.on_event,
                                  service->config.event_user);
  }
  if (rc != H2_PAL_OK) {
    bool stopping = true;
    if (lock_service(service) == H2_PAL_OK) {
      stopping = service->stopping;
      unlock_service(service);
    }
    if (!stopping)
      mark_terminal(service, rc);
    else
      complete_queued_as_closed(service);
    close_client(service);
    return;
  }

  for (;;) {
    if (lock_service(service) != H2_PAL_OK)
      break;
    const bool stopping = service->stopping;
    unlock_service(service);
    if (stopping)
      break;

    if (service->config.on_event != NULL) {
      rc = client_dispatch_event(service->client);
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
          rc != H2_PAL_ERR_WOULD_BLOCK) {
        mark_terminal(service, rc);
        break;
      }
    }

    h2_gizclaw_operation_t *operation = NULL;
    rc = (h2_pal_result_t)h2_pal_queue_recv(
        service->config.queue, service->request_queue, &operation,
        (uint32_t)service->config.client_poll_timeout_ms);
    if (rc == H2_PAL_OK) {
      if (lock_service(service) != H2_PAL_OK)
        break;
      service->current = operation;
      operation->state = H2_GIZCLAW_OPERATION_RUNNING;
      const bool canceled = operation->cancel_requested;
      unlock_service(service);

      h2_pal_result_t operation_rc = H2_PAL_ERR_CLOSED;
      if (!canceled)
        operation_rc = operation->run(operation->user, service->client,
                                      &operation->cancel_token);

      if (lock_service(service) != H2_PAL_OK)
        break;
      service->current = NULL;
      const bool transport_closed = operation_rc == H2_PAL_ERR_CLOSED;
      const bool terminal_failure = transport_closed &&
                                    !operation->cancel_requested &&
                                    !service->stopping;
      if (terminal_failure) {
        service->stopping = true;
        service->terminal_pending = true;
        service->terminal_result = operation_rc;
      }
      const bool service_closed = service->stopping;
      const bool operation_canceled = operation->cancel_requested;
      if (service_closed) {
        (void)enqueue_completion_locked(service, operation,
                                        H2_GIZCLAW_OPERATION_SERVICE_CLOSED,
                                        H2_PAL_ERR_CLOSED);
      } else if (operation_canceled) {
        (void)enqueue_completion_locked(service, operation,
                                        H2_GIZCLAW_OPERATION_CANCELED,
                                        H2_PAL_ERR_CLOSED);
      } else {
        (void)enqueue_completion_locked(
            service, operation, H2_GIZCLAW_OPERATION_FINISHED, operation_rc);
      }
      unlock_service(service);
      if (terminal_failure) {
        complete_queued_as_closed(service);
        break;
      }
      continue;
    }
    if (rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_WOULD_BLOCK) {
      mark_terminal(service, rc);
      break;
    }
    rc = client_poll(service->client, service->config.client_poll_timeout_ms);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
      rc = H2_PAL_OK;
    if (rc != H2_PAL_OK) {
      mark_terminal(service, rc);
      break;
    }
  }

  complete_queued_as_closed(service);
  close_client(service);
}

static bool valid_config(const h2_gizclaw_service_config_t *config) {
  return config != NULL && config->client_config != NULL &&
         config->client_config->allocator != NULL && config->task != NULL &&
         config->queue != NULL && config->sync != NULL &&
         config->operation_capacity > 0u &&
         config->operation_capacity <= UINT32_MAX &&
         config->client_poll_timeout_ms > 0;
}

h2_pal_result_t
h2_gizclaw_service_init(const h2_gizclaw_service_config_t *config,
                        h2_gizclaw_service_t **out_service) {
  if (!valid_config(config) || out_service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_service = NULL;
  h2_gizclaw_service_t *service =
      h2_pal_mem_alloc(config->client_config->allocator, sizeof(*service));
  if (service == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(service, 0, sizeof(*service));
  service->config = *config;
  service->config.task_options.name = h2_gizclaw_service_task_name;
  service->client_config = *config->client_config;
  service->original_cancel = service->client_config.cancel_requested;
  service->original_cancel_user = service->client_config.cancel_user;
  service->client_config.cancel_requested = service_cancel_requested;
  service->client_config.cancel_user = service;
  service->config.client_config = &service->client_config;

  const h2_pal_mutex_config_t mutex_config = {
      .name = "gizclaw-service", .allocator = config->client_config->allocator};
  h2_pal_result_t rc =
      h2_pal_mutex_create(config->sync, &mutex_config, &service->mutex);
  if (rc != H2_PAL_OK)
    goto fail;
  const h2_pal_cond_config_t cond_config = {
      .name = "gizclaw-progress",
      .allocator = config->client_config->allocator,
  };
  rc = h2_pal_cond_create(config->sync, &cond_config, &service->progress_cond);
  if (rc != H2_PAL_OK)
    goto fail;
  const h2_pal_queue_config_t request_config = {
      .name = "gizclaw-request",
      .item_size = sizeof(h2_gizclaw_operation_t *),
      .item_count = config->operation_capacity,
      .allocator = config->client_config->allocator,
  };
  rc = (h2_pal_result_t)h2_pal_queue_create(config->queue, &request_config,
                                            &service->request_queue);
  if (rc != H2_PAL_OK)
    goto fail;
  const h2_pal_queue_config_t completion_config = {
      .name = "gizclaw-completion",
      .item_size = sizeof(h2_gizclaw_operation_t *),
      .item_count = config->operation_capacity,
      .allocator = config->client_config->allocator,
  };
  rc = (h2_pal_result_t)h2_pal_queue_create(config->queue, &completion_config,
                                            &service->completion_queue);
  if (rc != H2_PAL_OK)
    goto fail;
  *out_service = service;
  return H2_PAL_OK;

fail:
  if (service->completion_queue != NULL)
    h2_pal_queue_destroy(config->queue, service->completion_queue);
  if (service->request_queue != NULL)
    h2_pal_queue_destroy(config->queue, service->request_queue);
  if (service->progress_cond != NULL)
    (void)h2_pal_cond_destroy(config->sync, service->progress_cond);
  if (service->mutex != NULL)
    (void)h2_pal_mutex_destroy(config->sync, service->mutex);
  h2_pal_mem_free(config->client_config->allocator, service);
  return rc;
}

h2_pal_result_t h2_gizclaw_service_start(h2_gizclaw_service_t *service) {
  if (service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock_service(service);
  if (rc != H2_PAL_OK)
    return rc;
  if (service->started || service->stopping || service->stopped) {
    unlock_service(service);
    return H2_PAL_ERR_INVALID_STATE;
  }
  service->started = true;
  unlock_service(service);
  rc = h2_pal_task_start(service->config.task, &service->config.task_options,
                         service_worker, service, &service->task);
  if (rc != H2_PAL_OK) {
    (void)lock_service(service);
    service->started = false;
    unlock_service(service);
  }
  return rc;
}

h2_pal_result_t
h2_gizclaw_service_submit(h2_gizclaw_service_t *service, uint64_t identity,
                          h2_gizclaw_operation_run_fn run,
                          h2_gizclaw_operation_completion_fn completion,
                          void *user, h2_gizclaw_operation_t **out_operation) {
  if (service == NULL || run == NULL || completion == NULL ||
      out_operation == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_operation = NULL;
  h2_pal_result_t rc = lock_service(service);
  if (rc != H2_PAL_OK)
    return rc;
  if (!service->started || service->stopping || service->stopped) {
    unlock_service(service);
    return service->started ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_INVALID_STATE;
  }
  if (service->active_count >= service->config.operation_capacity) {
    unlock_service(service);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  h2_gizclaw_operation_t *operation = h2_pal_mem_alloc(
      service->config.client_config->allocator, sizeof(*operation));
  if (operation == NULL) {
    unlock_service(service);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(operation, 0, sizeof(*operation));
  operation->service = service;
  operation->run = run;
  operation->completion = completion;
  operation->user = user;
  operation->result.identity = identity;
  operation->state = H2_GIZCLAW_OPERATION_QUEUED;
  operation->caller_reference = true;
  operation->internal_reference = true;
  operation->cancel_token.operation = operation;
  ++service->active_count;
  ++service->caller_reference_count;
  rc = (h2_pal_result_t)h2_pal_queue_send(service->config.queue,
                                          service->request_queue, &operation,
                                          H2_PAL_QUEUE_NO_WAIT);
  if (rc != H2_PAL_OK) {
    --service->active_count;
    --service->caller_reference_count;
    h2_pal_mem_free(service->config.client_config->allocator, operation);
    unlock_service(service);
    return rc == H2_PAL_ERR_FULL ? H2_PAL_ERR_WOULD_BLOCK : rc;
  }
  *out_operation = operation;
  unlock_service(service);
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_operation_cancel(h2_gizclaw_operation_t *operation) {
  if (operation == NULL || operation->service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_service_t *service = operation->service;
  h2_pal_result_t rc = lock_service(service);
  if (rc != H2_PAL_OK)
    return rc;
  if (operation->state == H2_GIZCLAW_OPERATION_QUEUED ||
      operation->state == H2_GIZCLAW_OPERATION_RUNNING ||
      operation->progress_pending) {
    operation->cancel_requested = true;
    (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
  }
  unlock_service(service);
  return H2_PAL_OK;
}

void h2_gizclaw_operation_release(h2_gizclaw_operation_t *operation) {
  if (operation == NULL || operation->service == NULL)
    return;
  h2_gizclaw_service_t *service = operation->service;
  if (lock_service(service) != H2_PAL_OK)
    return;
  if (operation->caller_reference) {
    operation->caller_reference = false;
    --service->caller_reference_count;
  }
  free_operation_if_unreferenced(operation);
  unlock_service(service);
}

h2_pal_result_t h2_gizclaw_service_dispatch(h2_gizclaw_service_t *service,
                                            size_t max_callbacks,
                                            size_t *out_dispatched) {
  if (out_dispatched != NULL)
    *out_dispatched = 0u;
  if (service == NULL || max_callbacks == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock_service(service);
  if (rc != H2_PAL_OK)
    return rc;
  if (service->dispatching) {
    unlock_service(service);
    return H2_PAL_ERR_INVALID_STATE;
  }
  service->dispatching = true;
  unlock_service(service);

  size_t dispatched = 0u;
  while (dispatched < max_callbacks) {
    h2_gizclaw_operation_t *operation = NULL;
    rc = (h2_pal_result_t)h2_pal_queue_recv(service->config.queue,
                                            service->completion_queue,
                                            &operation, H2_PAL_QUEUE_NO_WAIT);
    if (rc != H2_PAL_OK)
      break;
    if (lock_service(service) != H2_PAL_OK)
      break;
    operation->completion_queued = false;
    const bool progress =
        operation->state == H2_GIZCLAW_OPERATION_PROGRESS_PENDING ||
        operation->state == H2_GIZCLAW_OPERATION_DISPATCHING;
    const bool skip_progress =
        progress && (service->stopping || operation->cancel_requested ||
                     !operation->progress_pending);
    if (progress && !skip_progress) {
      operation->progress_claimed = true;
      operation->state = H2_GIZCLAW_OPERATION_DISPATCHING;
    }
    h2_gizclaw_operation_dispatch_fn progress_callback = operation->progress;
    void *progress_user = operation->progress_user;
    if (skip_progress)
      (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
    unlock_service(service);
    if (progress) {
      if (skip_progress) {
        ++dispatched;
        continue;
      }
      const h2_pal_result_t progress_result =
          progress_callback != NULL ? progress_callback(progress_user)
                                    : H2_PAL_ERR_INVALID_STATE;
      if (lock_service(service) != H2_PAL_OK)
        break;
      operation->progress_result = progress_result;
      operation->progress_pending = false;
      operation->progress_claimed = false;
      operation->state = H2_GIZCLAW_OPERATION_RUNNING;
      (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
      unlock_service(service);
      ++dispatched;
      continue;
    }
    operation->completion(operation->user, operation, &operation->result);
    if (lock_service(service) != H2_PAL_OK)
      break;
    operation->state = H2_GIZCLAW_OPERATION_TERMINAL;
    operation->internal_reference = false;
    --service->active_count;
    free_operation_if_unreferenced(operation);
    unlock_service(service);
    ++dispatched;
  }

  h2_gizclaw_service_terminal_fn terminal = NULL;
  void *terminal_user = NULL;
  h2_pal_result_t terminal_result = H2_PAL_OK;
  if (lock_service(service) == H2_PAL_OK) {
    service->dispatching = false;
    if (service->terminal_pending && !service->terminal_dispatched &&
        service->active_count == 0u) {
      service->terminal_dispatched = true;
      terminal = service->config.terminal;
      terminal_user = service->config.terminal_user;
      terminal_result = service->terminal_result;
    }
    unlock_service(service);
  }
  if (terminal != NULL)
    terminal(terminal_user, terminal_result);
  if (out_dispatched != NULL)
    *out_dispatched = dispatched;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_service_stop(h2_gizclaw_service_t *service) {
  if (service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock_service(service);
  if (rc != H2_PAL_OK)
    return rc;
  if (service->dispatching) {
    unlock_service(service);
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (service->stopped) {
    unlock_service(service);
    return H2_PAL_OK;
  }
  if (!service->started) {
    service->stopped = true;
    unlock_service(service);
    return H2_PAL_OK;
  }
  service->stopping = true;
  (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
  unlock_service(service);
  (void)h2_pal_queue_close(service->config.queue, service->request_queue);
  rc = h2_pal_task_join(service->config.task, service->task);
  if (rc != H2_PAL_OK)
    return rc;
  service->task = NULL;
  if (lock_service(service) != H2_PAL_OK)
    return H2_PAL_ERR_INVALID_STATE;
  service->stopped = true;
  unlock_service(service);
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_service_deinit(h2_gizclaw_service_t *service) {
  if (service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock_service(service);
  if (rc != H2_PAL_OK)
    return rc;
  if (!service->stopped || service->active_count != 0u ||
      service->caller_reference_count != 0u || service->dispatching) {
    unlock_service(service);
    return H2_PAL_ERR_INVALID_STATE;
  }
  unlock_service(service);
  h2_pal_queue_destroy(service->config.queue, service->completion_queue);
  h2_pal_queue_destroy(service->config.queue, service->request_queue);
  (void)h2_pal_cond_destroy(service->config.sync, service->progress_cond);
  (void)h2_pal_mutex_destroy(service->config.sync, service->mutex);
  h2_pal_mem_free(service->config.client_config->allocator, service);
  return H2_PAL_OK;
}
