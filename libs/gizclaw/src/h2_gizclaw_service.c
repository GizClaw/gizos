#include "h2_gizclaw_audio_pacer.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_pcm_track_internal.h"
#include "h2_gizclaw_service_internal.h"

#include "h2_gizclaw_task_names.h"

#include <stdio.h>
#include <string.h>

#ifdef H2_GIZCLAW_TESTING
static const h2_gizclaw_service_client_ops_t *s_client_ops;
static h2_gizclaw_runtime_notify_test_fn s_runtime_notify;

void h2_gizclaw_service_test_set_client_ops(
    const h2_gizclaw_service_client_ops_t *ops) {
  s_client_ops = ops;
}

void h2_gizclaw_service_test_set_runtime_notify(
    h2_gizclaw_runtime_notify_test_fn notify) {
  s_runtime_notify = notify;
}
#endif

void h2_gizclaw_service_log_request(const h2_gizclaw_service_t *service,
                                    h2_pal_log_level_t level,
                                    const char *request_kind, const char *stage,
                                    uint64_t identity, h2_pal_result_t result,
                                    int detail_code, size_t frame_count,
                                    size_t byte_count) {
  if (service == NULL || service->config.client_config == NULL ||
      service->config.client_config->log == NULL || request_kind == NULL ||
      stage == NULL)
    return;
  char message[192];
  (void)snprintf(message, sizeof(message),
                 "request=%s stage=%s identity=%llu rc=%d detail=%d "
                 "frames=%zu bytes=%zu",
                 request_kind, stage, (unsigned long long)identity, (int)result,
                 detail_code, frame_count, byte_count);
  (void)h2_pal_log_write(service->config.client_config->log, level, "gizclaw",
                         message);
}

static uint64_t service_monotonic_ms(const h2_gizclaw_service_t *service) {
  uint64_t now_ms = 0u;
  if (service == NULL || service->config.client_config == NULL ||
      service->config.client_config->time == NULL ||
      h2_pal_time_get_monotonic_ms(service->config.client_config->time,
                                   &now_ms) != H2_PAL_OK) {
    return 0u;
  }
  return now_ms;
}

static uint64_t elapsed_ms(uint64_t started_ms, uint64_t completed_ms) {
  return started_ms != 0u && completed_ms >= started_ms
             ? completed_ms - started_ms
             : 0u;
}

static void log_operation_trace(const h2_gizclaw_service_t *service,
                                h2_pal_log_level_t level, const char *stage,
                                const h2_gizclaw_operation_t *operation,
                                h2_pal_result_t result, int detail_code,
                                uint64_t dispatch_ms) {
  if (service == NULL || service->config.client_config == NULL ||
      service->config.client_config->log == NULL || stage == NULL ||
      operation == NULL) {
    return;
  }
  const uint64_t now_ms = service_monotonic_ms(service);
  const uint64_t completed_ms =
      operation->completed_at_ms != 0u ? operation->completed_at_ms : now_ms;
  char message[256];
  (void)snprintf(
      message, sizeof(message),
      "request=service stage=%s seq=%llu identity=%llu rc=%d detail=%d "
      "queue_ms=%llu run_ms=%llu total_ms=%llu polls=%lu "
      "dispatch_ms=%llu",
      stage, (unsigned long long)operation->trace_sequence,
      (unsigned long long)operation->result.identity, (int)result, detail_code,
      (unsigned long long)elapsed_ms(operation->queued_at_ms,
                                     operation->started_at_ms),
      (unsigned long long)elapsed_ms(operation->started_at_ms, completed_ms),
      (unsigned long long)elapsed_ms(operation->queued_at_ms, now_ms),
      (unsigned long)operation->poll_count, (unsigned long long)dispatch_ms);
  (void)h2_pal_log_write(service->config.client_config->log, level, "gizclaw",
                         message);
}

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
                         h2_gizclaw_client_event_sink_fn event,
                         void *event_user) {
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

static bool dispatch_work_ready_locked(const h2_gizclaw_service_t *service) {
  const h2_gizclaw_stream_ring_t *ring =
      &service->stream_rings[H2_GIZCLAW_STREAM_DATA_DOWNLINK];
  return service->dispatch_item_count != 0u ||
         (service->data_downlink_stream != NULL && ring->dispatch_ready &&
          ring->queued_frames != 0u) ||
         (service->terminal_pending && !service->terminal_dispatched &&
          service->active_count == 0u && service->config.terminal != NULL);
}

void h2_gizclaw_service_wake_dispatch_internal(h2_gizclaw_service_t *service) {
  if (service == NULL || service->config.runtime == NULL)
    return;
  /* Level signal: the Runtime keeps at most one wake pending and the App
   * drains this service after every wake, so nothing here needs to
   * remember whether a wake is already in flight. */
#ifdef H2_GIZCLAW_TESTING
  if (s_runtime_notify != NULL) {
    s_runtime_notify(service->config.runtime);
    return;
  }
#endif
  (void)h2_runtime_notify(service->config.runtime);
}

static bool service_cancel_requested(void *user) {
  h2_gizclaw_service_t *service = user;
  h2_gizclaw_cancel_fn original = NULL;
  void *original_user = NULL;
  bool canceled = true;
  if (service != NULL && lock_service(service) == H2_PAL_OK) {
    /* The SDK cancel hook describes the lifetime of the shared client/Peer.
     * A request owns a separate cancel token; treating that request-local
     * signal as client cancellation makes gzc_client_poll() close every
     * sibling channel and the event stream. */
    canceled = service->stopping;
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

static bool original_cancel_requested(h2_gizclaw_service_t *service) {
  h2_gizclaw_cancel_fn original = NULL;
  void *original_user = NULL;
  if (service != NULL && lock_service(service) == H2_PAL_OK) {
    original = service->original_cancel;
    original_user = service->original_cancel_user;
    unlock_service(service);
  }
  return original != NULL && original(original_user);
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
  /* No callback means no dispatch capacity is needed after terminal. Release
   * the slot before waking a synchronous caller that may submit the next RPC.
   */
  if (operation->completion == NULL) {
    --service->active_count;
    if (operation->settle != NULL)
      operation->settle(operation->user, operation, &operation->result);
    operation->state = H2_GIZCLAW_OPERATION_TERMINAL;
    atomic_store_explicit(&operation->terminal, true, memory_order_release);
    return H2_PAL_OK;
  }
  if (operation->dispatch_queued)
    return H2_PAL_OK;
  const h2_gizclaw_dispatch_item_t item = {
      .kind = H2_GIZCLAW_DISPATCH_OPERATION,
      .operation = operation,
  };
  const int rc =
      h2_pal_queue_send(service->config.queue, service->dispatch_queue, &item,
                        H2_PAL_QUEUE_NO_WAIT);
  if (rc == H2_PAL_OK) {
    operation->dispatch_queued = true;
    ++service->dispatch_item_count;
    if (operation->settle != NULL)
      operation->settle(operation->user, operation, &operation->result);
    h2_gizclaw_service_wake_dispatch_internal(service);
    return H2_PAL_OK;
  }

  /* Completion is only an App hook. Never hold the network owner waiting for
   * caller dispatch capacity: fail this request locally, wake its waiters and
   * retire it without invoking the hook. */
  const h2_pal_result_t overflow = H2_PAL_ERR_WOULD_BLOCK;
  operation->result.result = overflow;
  operation->completion = NULL;
  --service->active_count;
  if (operation->settle != NULL)
    operation->settle(operation->user, operation, &operation->result);
  operation->state = H2_GIZCLAW_OPERATION_TERMINAL;
  atomic_store_explicit(&operation->terminal, true, memory_order_release);
  h2_gizclaw_service_log_request(
      service, H2_PAL_LOG_WARN, "completion", "queue_full",
      operation->result.identity, overflow,
      (int)service->config.operation_capacity, service->dispatch_item_count,
      service->active_count);
  return overflow;
}

static void retire_operation(h2_gizclaw_service_t *service,
                             h2_gizclaw_operation_t *operation) {
  /* The operation's internal reference protects it if this drops the last
   * request reference and releases its caller-owned operation handle. */
  if (operation->release_user != NULL)
    operation->release_user(operation->user);
  if (lock_service(service) != H2_PAL_OK)
    return;
  operation->internal_reference = false;
  if (operation->completion != NULL)
    --service->active_count;
  const bool wake_terminal =
      service->active_count == 0u && service->terminal_pending &&
      !service->terminal_dispatched && service->config.terminal != NULL;
  free_operation_if_unreferenced(operation);
  unlock_service(service);
  if (wake_terminal)
    h2_gizclaw_service_wake_dispatch_internal(service);
}

h2_pal_result_t h2_gizclaw_service_post_internal(h2_gizclaw_service_t *service,
                                                 void (*notify)(void *user),
                                                 void *user) {
  if (service == NULL || notify == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock_service(service);
  if (rc != H2_PAL_OK)
    return rc;
  if (!service->started || service->stopping || service->stopped) {
    rc = H2_PAL_ERR_CLOSED;
  } else if (service->queued_event_count >=
             service->config.operation_capacity) {
    rc = H2_PAL_ERR_WOULD_BLOCK;
  } else {
    const h2_gizclaw_dispatch_item_t item = {
        .kind = H2_GIZCLAW_DISPATCH_NOTIFICATION,
        .notify = notify,
        .notify_user = user,
    };
    rc = h2_pal_queue_send(service->config.queue, service->dispatch_queue,
                           &item, H2_PAL_QUEUE_NO_WAIT);
    if (rc == H2_PAL_OK) {
      ++service->queued_event_count;
      ++service->dispatch_item_count;
      h2_gizclaw_service_wake_dispatch_internal(service);
    }
  }
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
    if (operation->finish != NULL)
      operation->finish(operation->user);
    if (lock_service(service) != H2_PAL_OK)
      return;
    (void)enqueue_completion_locked(service, operation,
                                    H2_GIZCLAW_OPERATION_SERVICE_CLOSED,
                                    H2_PAL_ERR_CLOSED);
    const bool dispatch = operation->completion != NULL;
    unlock_service(service);
    if (!dispatch)
      retire_operation(service, operation);
  }
}

static void mark_terminal(h2_gizclaw_service_t *service,
                          h2_pal_result_t result) {
  if (lock_service(service) != H2_PAL_OK)
    return;
  bool newly_terminal = false;
  size_t active_count = service->active_count;
  if (!service->stopping) {
    service->stopping = true;
    service->terminal_pending = true;
    service->terminal_result = result;
    newly_terminal = true;
  }
  unlock_service(service);
  if (newly_terminal) {
    h2_gizclaw_service_wake_dispatch_internal(service);
    h2_gizclaw_service_log_request(service, H2_PAL_LOG_ERROR, "service",
                                   "transport_terminal", 0u, result,
                                   (int)active_count, 0u, 0u);
  }
  complete_queued_as_closed(service);
}

static bool complete_operation(h2_gizclaw_service_t *service,
                               h2_gizclaw_operation_t *operation,
                               h2_pal_result_t operation_rc,
                               bool original_canceled) {
  if (operation->finish != NULL)
    operation->finish(operation->user);
  operation->completed_at_ms = service_monotonic_ms(service);
  if (lock_service(service) != H2_PAL_OK)
    return true;
  if (service->current == operation)
    service->current = NULL;
  /* CLOSED can describe a request/Track, not the Peer. Transport liveness
   * belongs to client_poll; only an explicit service-wide cancellation may
   * turn an operation result into a service terminal here. */
  const bool terminal_failure =
      operation_rc == H2_PAL_ERR_CLOSED && original_canceled;
  const int close_detail = (operation->cancel_requested ? 1 : 0) |
                           (original_canceled ? 2 : 0) |
                           (service->stopping ? 4 : 0);
  if (terminal_failure) {
    service->stopping = true;
    service->terminal_pending = true;
    service->terminal_result = operation_rc;
  }
  if (service->stopping) {
    (void)enqueue_completion_locked(service, operation,
                                    H2_GIZCLAW_OPERATION_SERVICE_CLOSED,
                                    H2_PAL_ERR_CLOSED);
  } else if (operation->cancel_requested) {
    (void)enqueue_completion_locked(
        service, operation, H2_GIZCLAW_OPERATION_CANCELED, H2_PAL_ERR_CLOSED);
  } else {
    (void)enqueue_completion_locked(
        service, operation, H2_GIZCLAW_OPERATION_FINISHED, operation_rc);
  }
  const bool dispatch = operation->completion != NULL;
  const h2_gizclaw_operation_t trace = *operation;
  unlock_service(service);
  log_operation_trace(
      service,
      terminal_failure
          ? H2_PAL_LOG_ERROR
          : (operation_rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_WARN),
      terminal_failure
          ? "operation_closed_service"
          : (operation_rc == H2_PAL_OK ? "operation_completed"
                                       : "operation_completed_error"),
      &trace, operation_rc, close_detail, 0u);
  if (!dispatch)
    retire_operation(service, operation);
  return terminal_failure;
}

static bool poll_pending_operations(h2_gizclaw_service_t *service) {
  h2_gizclaw_operation_t **cursor = &service->pending;
  while (*cursor != NULL) {
    h2_gizclaw_operation_t *operation = *cursor;
    if (operation->notification_driven && !operation->ready &&
        !operation->cancel_requested) {
      cursor = &operation->next_pending;
      continue;
    }
    operation->ready = false;
    ++operation->poll_count;
    const h2_pal_result_t rc = operation->poll(operation->user, service->client,
                                               &operation->cancel_token);
    if (rc == H2_PAL_ERR_WOULD_BLOCK) {
      cursor = &operation->next_pending;
      continue;
    }
    *cursor = operation->next_pending;
    operation->next_pending = NULL;
    const bool original_canceled =
        rc == H2_PAL_ERR_CLOSED && original_cancel_requested(service);
    if (complete_operation(service, operation, rc, original_canceled))
      return false;
  }
  return true;
}

static void complete_pending_as_closed(h2_gizclaw_service_t *service) {
  h2_gizclaw_operation_t *operation = service->pending;
  service->pending = NULL;
  while (operation != NULL) {
    h2_gizclaw_operation_t *next = operation->next_pending;
    operation->next_pending = NULL;
    if (lock_service(service) == H2_PAL_OK) {
      operation->cancel_requested = true;
      unlock_service(service);
    }
    (void)operation->poll(operation->user, service->client,
                          &operation->cancel_token);
    (void)complete_operation(service, operation, H2_PAL_ERR_CLOSED, false);
    operation = next;
  }
}

static h2_pal_result_t
queue_client_event(void *user, const h2_gizclaw_client_event_t *event) {
  h2_gizclaw_service_t *service = user;
  if (service == NULL || event == NULL ||
      (event->workspace_name.len > 0 && event->workspace_name.data == NULL) ||
      event->workspace_name.len == SIZE_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock_service(service);
  if (rc != H2_PAL_OK)
    return rc;
  if (service->stopping) {
    unlock_service(service);
    return H2_PAL_ERR_CLOSED;
  }
  if (service->queued_event_count >= service->config.operation_capacity) {
    unlock_service(service);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  h2_gizclaw_dispatch_item_t item = {.kind = H2_GIZCLAW_DISPATCH_CLIENT_EVENT,
                                     .event = *event};
  if (event->workspace_name.len > 0) {
    item.event_workspace_name = h2_pal_mem_alloc(
        service->client_config.allocator, event->workspace_name.len + 1);
    if (item.event_workspace_name == NULL) {
      unlock_service(service);
      return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(item.event_workspace_name, event->workspace_name.data,
           event->workspace_name.len);
    item.event_workspace_name[event->workspace_name.len] = '\0';
    item.event.workspace_name.data = item.event_workspace_name;
  }
  rc = h2_pal_queue_send(service->config.queue, service->dispatch_queue, &item,
                         H2_PAL_QUEUE_NO_WAIT);
  if (rc == H2_PAL_OK) {
    ++service->queued_event_count;
    ++service->dispatch_item_count;
    h2_gizclaw_service_wake_dispatch_internal(service);
  }
  unlock_service(service);
  if (rc != H2_PAL_OK)
    h2_pal_mem_free(service->client_config.allocator,
                    item.event_workspace_name);
  return rc;
}

static void uplink_worker(void *ctx) {
  h2_gizclaw_service_t *service = ctx;
  uint64_t deadline = 0u;
  uint64_t previous = 0u;
  bool pacing = false;
  h2_pal_result_t rc;
  for (;;) {
    if (lock_service(service) != H2_PAL_OK)
      return;
    uint64_t started = previous;
    while (!service->stopping) {
      if (atomic_load(&service->speech_request) == NULL &&
          atomic_load(&service->media_request) == NULL) {
        pacing = false;
        (void)h2_pal_cond_wait(service->config.sync, service->progress_cond,
                               service->mutex, H2_GIZCLAW_AUDIO_PERIOD_MS);
        continue;
      }
      rc = h2_pal_time_get_monotonic_ms(service->client_config.time, &started);
      if (rc != H2_PAL_OK || (pacing && started < previous)) {
        unlock_service(service);
        mark_terminal(service, rc != H2_PAL_OK ? rc : H2_PAL_ERR_INVALID_STATE);
        return;
      }
      if (!pacing) {
        deadline = started;
        pacing = true;
      }
      previous = started;
      if (started >= deadline)
        break;
      /* Notifications may wake us for stop/route changes, but must not cause
       * a new PCM frame before its scheduled deadline. */
      (void)h2_pal_cond_wait(service->config.sync, service->progress_cond,
                             service->mutex, (uint32_t)(deadline - started));
    }
    const bool stopping = service->stopping;
    unlock_service(service);
    if (stopping)
      return;
    h2_gizclaw_speech_uplink_step_internal(service);
    h2_gizclaw_conversation_uplink_step_internal(service);
    (void)h2_gizclaw_req_data_step_internal(service,
                                            H2_GIZCLAW_STREAM_AUDIO_UPLINK);
    uint64_t completed = 0u;
    rc = h2_pal_time_get_monotonic_ms(service->client_config.time, &completed);
    if (rc != H2_PAL_OK || completed < started) {
      mark_terminal(service, rc != H2_PAL_OK ? rc : H2_PAL_ERR_INVALID_STATE);
      return;
    }
    const uint64_t elapsed = completed - started;
    if (elapsed > H2_GIZCLAW_AUDIO_PERIOD_MS) {
      char message[160];
      (void)snprintf(
          message, sizeof(message),
          "stage=uplink_cycle_overrun elapsed_ms=%llu overrun_ms=%llu "
          "period_ms=20",
          (unsigned long long)elapsed,
          (unsigned long long)(elapsed - H2_GIZCLAW_AUDIO_PERIOD_MS));
      (void)h2_pal_log_write(service->client_config.log, H2_PAL_LOG_WARN,
                             "gizclaw", message);
    }
    previous = completed;
    rc = h2_gizclaw_audio_next_deadline(deadline, completed, &deadline);
    if (rc != H2_PAL_OK) {
      mark_terminal(service, rc);
      return;
    }
  }
}

static void downlink_worker(void *ctx) {
  h2_gizclaw_service_t *service = ctx;
  for (;;) {
    if (lock_service(service) != H2_PAL_OK)
      return;
    if (service->stopping) {
      unlock_service(service);
      return;
    }
    unlock_service(service);
    h2_gizclaw_audio_play_downlink_step_internal(service);
    h2_gizclaw_conversation_downlink_step_internal(service);
    (void)h2_gizclaw_req_data_step_internal(service,
                                            H2_GIZCLAW_STREAM_AUDIO_DOWNLINK);
    if (lock_service(service) != H2_PAL_OK)
      return;
    /* Wake on progress, or after one audio period. The PCM Track drains one
     * chunk per period, so a 1 ms retry mostly re-locked the service to be
     * refused by a full Track, at the same priority and on the same core as
     * the speaker that had to free it. */
    if (!service->stopping)
      (void)h2_pal_cond_wait(service->config.sync, service->progress_cond,
                             service->mutex, H2_GIZCLAW_AUDIO_PERIOD_MS);
    unlock_service(service);
  }
}

static void data_worker(h2_gizclaw_service_t *service,
                        h2_gizclaw_stream_lane_t lane) {
  for (;;) {
    if (lock_service(service) != H2_PAL_OK)
      return;
    while (!service->stopping &&
           !h2_gizclaw_req_data_ready_internal(service, lane))
      (void)h2_pal_cond_wait(service->config.sync, service->progress_cond,
                             service->mutex, 1u);
    const bool stopping = service->stopping;
    unlock_service(service);
    if (stopping)
      return;
    (void)h2_gizclaw_req_data_step_internal(service, lane);
  }
}

static void data_uplink_worker(void *ctx) {
  data_worker(ctx, H2_GIZCLAW_STREAM_DATA_UPLINK);
}

static void data_downlink_worker(void *ctx) {
  data_worker(ctx, H2_GIZCLAW_STREAM_DATA_DOWNLINK);
}

static void net_worker(void *ctx) {
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
    rc = client_set_event_handler(service->client, queue_client_event, service);
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
    rc = (h2_pal_result_t)h2_pal_queue_recv(service->config.queue,
                                            service->request_queue, &operation,
                                            H2_PAL_QUEUE_NO_WAIT);
    if (rc == H2_PAL_OK) {
      if (lock_service(service) != H2_PAL_OK)
        break;
      service->current = operation;
      operation->state = H2_GIZCLAW_OPERATION_RUNNING;
      operation->started_at_ms = service_monotonic_ms(service);
      const bool canceled = operation->cancel_requested;
      unlock_service(service);

      log_operation_trace(service, H2_PAL_LOG_INFO, "request_started",
                          operation, H2_PAL_OK, operation->poll != NULL, 0u);

      h2_pal_result_t operation_rc = H2_PAL_ERR_CLOSED;
      if (!canceled)
        operation_rc = operation->run(operation->user, service->client,
                                      &operation->cancel_token);
      if (operation->poll != NULL && operation_rc == H2_PAL_ERR_WOULD_BLOCK) {
        if (lock_service(service) != H2_PAL_OK)
          break;
        service->current = NULL;
        operation->state = H2_GIZCLAW_OPERATION_PENDING;
        operation->next_pending = service->pending;
        service->pending = operation;
        unlock_service(service);
      } else {
        const bool original_canceled = operation_rc == H2_PAL_ERR_CLOSED &&
                                       original_cancel_requested(service);
        if (complete_operation(service, operation, operation_rc,
                               original_canceled)) {
          complete_queued_as_closed(service);
          break;
        }
      }
    } else if (rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_WOULD_BLOCK) {
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
    if (!poll_pending_operations(service)) {
      complete_queued_as_closed(service);
      break;
    }
  }

  complete_pending_as_closed(service);
  complete_queued_as_closed(service);
  close_client(service);
}

static bool dispatch_terminal_if_ready(h2_gizclaw_service_t *service,
                                       bool callback_budget_available) {
  h2_gizclaw_service_terminal_fn terminal = NULL;
  void *terminal_user = NULL;
  h2_pal_result_t terminal_result = H2_PAL_OK;
  if (lock_service(service) == H2_PAL_OK) {
    if (service->terminal_pending && !service->terminal_dispatched &&
        service->active_count == 0u &&
        (callback_budget_available || service->config.terminal == NULL)) {
      service->terminal_dispatched = true;
      terminal = service->config.terminal;
      terminal_user = service->config.terminal_user;
      terminal_result = service->terminal_result;
    }
    unlock_service(service);
  }
  if (terminal != NULL) {
    terminal(terminal_user, terminal_result);
    return true;
  }
  return false;
}

static void dispatch_operation(h2_gizclaw_service_t *service,
                               h2_gizclaw_operation_t *operation) {
  if (lock_service(service) != H2_PAL_OK)
    return;
  operation->dispatch_queued = false;
  operation->state = H2_GIZCLAW_OPERATION_TERMINAL;
  unlock_service(service);
  atomic_store_explicit(&operation->terminal, true, memory_order_release);
  log_operation_trace(service, H2_PAL_LOG_INFO, "dispatch_begin", operation,
                      operation->result.result,
                      (int)operation->result.terminal_kind, 0u);
  const uint64_t dispatch_started_ms = service_monotonic_ms(service);
  operation->completion(operation->user, operation, &operation->result);
  const uint64_t dispatch_finished_ms = service_monotonic_ms(service);
  log_operation_trace(service, H2_PAL_LOG_INFO, "dispatch_end", operation,
                      operation->result.result,
                      (int)operation->result.terminal_kind,
                      elapsed_ms(dispatch_started_ms, dispatch_finished_ms));
  retire_operation(service, operation);
}

h2_pal_result_t h2_gizclaw_service_poll(h2_gizclaw_service_t *service,
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
    if (h2_gizclaw_req_dispatch_output_internal(service)) {
      ++dispatched;
      continue;
    }
    h2_gizclaw_dispatch_item_t item;
    memset(&item, 0, sizeof(item));
    rc = lock_service(service);
    if (rc != H2_PAL_OK)
      break;
    rc = (h2_pal_result_t)h2_pal_queue_recv(service->config.queue,
                                            service->dispatch_queue, &item,
                                            H2_PAL_QUEUE_NO_WAIT);
    if (rc == H2_PAL_OK && service->dispatch_item_count != 0u)
      --service->dispatch_item_count;
    unlock_service(service);
    if (rc != H2_PAL_OK)
      break;
    if (item.kind == H2_GIZCLAW_DISPATCH_OPERATION) {
      dispatch_operation(service, item.operation);
    } else if (item.kind == H2_GIZCLAW_DISPATCH_CLIENT_EVENT ||
               item.kind == H2_GIZCLAW_DISPATCH_NOTIFICATION) {
      if (item.kind == H2_GIZCLAW_DISPATCH_NOTIFICATION) {
        item.notify(item.notify_user);
      } else if (service->config.on_event != NULL) {
        service->config.on_event(service->config.event_user, &item.event);
      }
      h2_pal_mem_free(service->config.client_config->allocator,
                      item.event_workspace_name);
      if (lock_service(service) == H2_PAL_OK) {
        --service->queued_event_count;
        (void)h2_pal_cond_broadcast(service->config.sync,
                                    service->progress_cond);
        unlock_service(service);
      }
    }
    ++dispatched;
  }
  if (dispatch_terminal_if_ready(service, dispatched < max_callbacks))
    ++dispatched;
  if (lock_service(service) != H2_PAL_OK)
    return H2_PAL_ERR_INVALID_STATE;
  service->dispatching = false;
  const bool dispatch_ready = dispatch_work_ready_locked(service);
  unlock_service(service);
  if (dispatch_ready)
    h2_gizclaw_service_wake_dispatch_internal(service);
  if (out_dispatched != NULL)
    *out_dispatched = dispatched;
  return rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK
                                                                  : rc;
}

static bool valid_config(const h2_gizclaw_service_config_t *config) {
  return config != NULL && config->client_config != NULL &&
         config->client_config->allocator != NULL && config->task != NULL &&
         config->client_config->time != NULL &&
         config->client_config->time->vtable != NULL &&
         config->client_config->time->vtable->get_monotonic_ms != NULL &&
         config->queue != NULL && config->sync != NULL &&
         config->operation_capacity > 0u &&
         config->operation_capacity <= UINT32_MAX &&
         config->operation_capacity <= (SIZE_MAX - 1u) / 2u &&
         config->client_poll_timeout_ms > 0;
}

static h2_pal_result_t service_track_read_opus(void *user, uint8_t *opus,
                                               size_t capacity,
                                               size_t *out_len) {
  return h2_gizclaw_service_media_read_opus((h2_gizclaw_service_t *)user, opus,
                                            capacity, out_len);
}

static h2_pal_result_t service_track_write_opus(void *user, const uint8_t *opus,
                                                size_t opus_len) {
  return h2_gizclaw_service_media_write_opus((h2_gizclaw_service_t *)user, opus,
                                             opus_len);
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
  /* This also initializes the four Service-owned streaming rings. */
  memset(service, 0, sizeof(*service));
  service->config = *config;
  service->config.net_task_options.name = h2_gizclaw_net_task_name;
  service->client_config = *config->client_config;
  service->original_cancel = service->client_config.cancel_requested;
  service->original_cancel_user = service->client_config.cancel_user;
  service->client_config.cancel_requested = service_cancel_requested;
  service->client_config.cancel_user = service;
  service->webrtc_track_vtable = (h2_pal_webrtc_track_vtable_t){
      .read = service_track_read_opus,
      .write = service_track_write_opus,
  };
  service->webrtc_track = (h2_pal_webrtc_track_t){
      .user = service,
      .vtable = &service->webrtc_track_vtable,
  };
  service->client_config.webrtc_media_track = &service->webrtc_track;
  service->config.client_config = &service->client_config;
  atomic_init(&service->media_request, NULL);
  atomic_init(&service->speech_request, NULL);
  atomic_init(&service->pcm_track, NULL);
  atomic_init(&service->media_callback_refs, 0u);
  atomic_init(&service->media_holder_tag, 0);

  const h2_pal_mutex_config_t mutex_config = {
      .name = "gizclaw-service", .allocator = config->client_config->allocator};
  h2_pal_result_t rc =
      h2_pal_mutex_create(config->sync, &mutex_config, &service->mutex);
  if (rc != H2_PAL_OK)
    goto fail;
  rc = h2_pal_mutex_create(config->sync, &mutex_config, &service->audio_mutex);
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
  const h2_pal_queue_config_t dispatch_config = {
      .name = "gizclaw-dispatch",
      .item_size = sizeof(h2_gizclaw_dispatch_item_t),
      .item_count = config->operation_capacity * 2u + 1u,
      .allocator = config->client_config->allocator,
  };
  rc = (h2_pal_result_t)h2_pal_queue_create(config->queue, &dispatch_config,
                                            &service->dispatch_queue);
  if (rc != H2_PAL_OK)
    goto fail;
  *out_service = service;
  return H2_PAL_OK;

fail:
  if (service->audio_mutex != NULL)
    (void)h2_pal_mutex_destroy(config->sync, service->audio_mutex);
  if (service->dispatch_queue != NULL)
    h2_pal_queue_destroy(config->queue, service->dispatch_queue);
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
  rc =
      h2_pal_task_start(service->config.task, &service->config.net_task_options,
                        net_worker, service, &service->net_task);
  if (rc != H2_PAL_OK) {
    (void)lock_service(service);
    service->started = false;
    unlock_service(service);
    return rc;
  }
  const h2_pal_task_options_t uplink_options = {
      .name = h2_gizclaw_audio_uplink_task_name, .min_stack_size = 65536u};
  rc = h2_pal_task_start(service->config.task, &uplink_options, uplink_worker,
                         service, &service->uplink_task);
  if (rc == H2_PAL_OK) {
    const h2_pal_task_options_t downlink_options = {
        .name = h2_gizclaw_audio_downlink_task_name, .min_stack_size = 16384u};
    rc = h2_pal_task_start(service->config.task, &downlink_options,
                           downlink_worker, service, &service->downlink_task);
  }
  if (rc == H2_PAL_OK) {
    const h2_pal_task_options_t options = {.name = "$gizclaw/data-up",
                                           .min_stack_size = 16384u};
    rc = h2_pal_task_start(service->config.task, &options, data_uplink_worker,
                           service, &service->data_uplink_task);
  }
  if (rc == H2_PAL_OK) {
    const h2_pal_task_options_t options = {.name = "$gizclaw/data-down",
                                           .min_stack_size = 16384u};
    rc = h2_pal_task_start(service->config.task, &options, data_downlink_worker,
                           service, &service->data_downlink_task);
  }
  if (rc != H2_PAL_OK)
    (void)h2_gizclaw_service_stop(service);
  return rc;
}

h2_pal_result_t h2_gizclaw_service_set_track(h2_gizclaw_service_t *service,
                                             h2_gizclaw_track_t *track) {
  if (service == NULL || track == NULL || track->vtable == NULL ||
      (track->vtable->read == NULL && track->vtable->write == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock_service(service);
  if (rc != H2_PAL_OK)
    return rc;
  if (service->pcm_track_unsetting ||
      atomic_load(&service->pcm_track) != NULL ||
      atomic_load(&service->speech_request) != NULL ||
      service->audio_play != NULL ||
      atomic_load(&service->media_request) != NULL)
    rc = H2_PAL_ERR_INVALID_STATE;
  else {
    rc = h2_gizclaw_pcm_track_attach_internal(track);
    if (rc == H2_PAL_OK)
      atomic_store(&service->pcm_track, track);
  }
  unlock_service(service);
  return rc;
}

h2_pal_result_t h2_gizclaw_service_unset_track(h2_gizclaw_service_t *service,
                                               h2_gizclaw_track_t *track) {
  if (service == NULL || track == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock_service(service);
  if (rc != H2_PAL_OK)
    return rc;
  if (service->pcm_track_unsetting ||
      atomic_load(&service->pcm_track) != track) {
    unlock_service(service);
    return H2_PAL_ERR_INVALID_STATE;
  }
  service->pcm_track_unsetting = true;
  while (service->pcm_track_refs != 0u && rc == H2_PAL_OK)
    rc = h2_pal_cond_wait(service->config.sync, service->progress_cond,
                          service->mutex, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK) {
    atomic_store(&service->pcm_track, NULL);
    h2_gizclaw_pcm_track_detach_internal(track);
  }
  service->pcm_track_unsetting = false;
  unlock_service(service);
  return rc;
}

static h2_gizclaw_track_t *pcm_track_acquire(h2_gizclaw_service_t *service) {
  if (service == NULL || lock_service(service) != H2_PAL_OK)
    return NULL;
  h2_gizclaw_track_t *track =
      service->pcm_track_unsetting ? NULL : atomic_load(&service->pcm_track);
  if (track != NULL)
    ++service->pcm_track_refs;
  unlock_service(service);
  return track;
}

static void pcm_track_release(h2_gizclaw_service_t *service) {
  (void)lock_service(service);
  --service->pcm_track_refs;
  if (service->pcm_track_refs == 0u)
    (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
  unlock_service(service);
}

bool h2_gizclaw_service_pcm_readable_internal(h2_gizclaw_service_t *service) {
  h2_gizclaw_track_t *track = pcm_track_acquire(service);
  if (track == NULL)
    return false;
  bool readable = track->vtable->read != NULL;
  pcm_track_release(service);
  return readable;
}

h2_pal_result_t h2_gizclaw_service_pcm_input_internal(
    h2_gizclaw_service_t *service, h2_gizclaw_pcm_input_t *input,
    h2_gizclaw_pcm_input_action_t action, uint8_t *pcm, size_t len,
    size_t *out_len) {
  h2_gizclaw_track_t *track = pcm_track_acquire(service);
  if (track == NULL)
    return H2_PAL_ERR_CLOSED;
  h2_pal_result_t rc;
  switch (action) {
  case H2_GIZCLAW_PCM_INPUT_START:
    rc = h2_gizclaw_pcm_input_start(input, track,
                                    service->client_config.allocator);
    break;
  case H2_GIZCLAW_PCM_INPUT_END:
    rc = h2_gizclaw_pcm_input_end(input, track);
    break;
  case H2_GIZCLAW_PCM_INPUT_PREPARE:
    rc = h2_gizclaw_pcm_input_prepare(input, track);
    break;
  case H2_GIZCLAW_PCM_INPUT_READ:
    rc = h2_gizclaw_pcm_input_read(input, track, pcm, len, out_len);
    break;
  default:
    rc = H2_PAL_ERR_INVALID_ARG;
  }
  pcm_track_release(service);
  return rc;
}

h2_pal_result_t
h2_gizclaw_service_pcm_read_internal(h2_gizclaw_service_t *service,
                                     uint8_t *pcm, size_t capacity,
                                     size_t *out_len) {
  if (out_len != NULL)
    *out_len = 0u;
  if (service == NULL || pcm == NULL || capacity == 0u || out_len == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_track_t *track = pcm_track_acquire(service);
  if (track == NULL)
    return H2_PAL_ERR_CLOSED;
  h2_pal_result_t rc =
      track->vtable->read == NULL
          ? H2_PAL_ERR_UNSUPPORTED
          : track->vtable->read(track->user, pcm, capacity, out_len);
  if (rc == H2_PAL_OK && (*out_len == 0u || *out_len > capacity ||
                          *out_len % sizeof(int16_t) != 0u))
    rc = H2_PAL_ERR_FORMAT;
  if (rc != H2_PAL_OK)
    *out_len = 0u;
  pcm_track_release(service);
  return rc;
}

h2_pal_result_t
h2_gizclaw_service_pcm_write_internal(h2_gizclaw_service_t *service,
                                      const uint8_t *pcm, size_t len) {
  if (service == NULL || pcm == NULL || len == 0u ||
      len % sizeof(int16_t) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_track_t *track = pcm_track_acquire(service);
  if (track == NULL)
    return H2_PAL_ERR_CLOSED;
  h2_pal_result_t rc = track->vtable->write == NULL
                           ? H2_PAL_ERR_UNSUPPORTED
                           : track->vtable->write(track->user, pcm, len);
  pcm_track_release(service);
  return rc;
}

void h2_gizclaw_service_pcm_discard_downlink_internal(
    h2_gizclaw_service_t *service) {
  h2_gizclaw_track_t *track = pcm_track_acquire(service);
  if (track == NULL)
    return;
  h2_gizclaw_pcm_track_discard_downlink_internal(track);
  pcm_track_release(service);
}

bool h2_gizclaw_service_pcm_downlink_stats_internal(
    h2_gizclaw_service_t *service, size_t *out_used, size_t *out_capacity) {
  if (service == NULL || out_used == NULL || out_capacity == NULL)
    return false;
  h2_gizclaw_track_t *track = pcm_track_acquire(service);
  if (track == NULL)
    return false;
  const bool available = h2_gizclaw_pcm_track_downlink_stats_internal(
      track, out_used, out_capacity);
  pcm_track_release(service);
  return available;
}

static h2_pal_result_t submit_operation(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_operation_run_fn run, h2_gizclaw_operation_run_fn poll,
    h2_gizclaw_operation_completion_fn completion,
    h2_gizclaw_operation_completion_fn settle, void (*release_user)(void *user),
    void (*finish)(void *user), void *user, bool notification_driven,
    h2_gizclaw_operation_t **out_operation) {
  if (service == NULL || run == NULL ||
      (completion == NULL && settle == NULL) || out_operation == NULL)
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
  operation->poll = poll;
  operation->completion = completion;
  operation->settle = settle;
  operation->release_user = release_user;
  operation->finish = finish;
  operation->user = user;
  operation->result.identity = identity;
  operation->trace_sequence = ++service->next_trace_sequence;
  operation->queued_at_ms = service_monotonic_ms(service);
  operation->state = H2_GIZCLAW_OPERATION_QUEUED;
  operation->notification_driven = notification_driven;
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
  const size_t active_count = service->active_count;
  const h2_gizclaw_operation_t trace = *operation;
  unlock_service(service);
  log_operation_trace(service, H2_PAL_LOG_INFO, "request_queued", &trace,
                      H2_PAL_OK, (int)active_count, 0u);
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_service_submit_async_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_operation_run_fn start, h2_gizclaw_operation_run_fn poll,
    h2_gizclaw_operation_completion_fn completion, void *user,
    h2_gizclaw_operation_t **out_operation) {
  if (poll == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return submit_operation(service, identity, start, poll, completion, NULL,
                          NULL, NULL, user, false, out_operation);
}

h2_pal_result_t h2_gizclaw_service_submit_request_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_operation_run_fn start, h2_gizclaw_operation_run_fn poll,
    h2_gizclaw_operation_completion_fn settle,
    h2_gizclaw_operation_completion_fn completion,
    void (*release_user)(void *user), void (*finish)(void *user), void *user,
    bool notification_driven, h2_gizclaw_operation_t **out_operation) {
  if (settle == NULL || release_user == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return submit_operation(service, identity, start, poll, completion, settle,
                          release_user, finish, user, notification_driven,
                          out_operation);
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
      operation->state == H2_GIZCLAW_OPERATION_PENDING) {
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

h2_pal_result_t h2_gizclaw_service_stop(h2_gizclaw_service_t *service) {
  if (service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock_service(service);
  if (rc != H2_PAL_OK)
    return rc;
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
  if (service->net_task != NULL) {
    rc = h2_pal_task_join(service->config.task, service->net_task);
    if (rc != H2_PAL_OK)
      return rc;
    service->net_task = NULL;
  }
  if (service->uplink_task != NULL) {
    rc = h2_pal_task_join(service->config.task, service->uplink_task);
    if (rc != H2_PAL_OK)
      return rc;
    service->uplink_task = NULL;
  }
  if (service->downlink_task != NULL) {
    rc = h2_pal_task_join(service->config.task, service->downlink_task);
    if (rc != H2_PAL_OK)
      return rc;
    service->downlink_task = NULL;
  }
  if (service->data_uplink_task != NULL) {
    rc = h2_pal_task_join(service->config.task, service->data_uplink_task);
    if (rc != H2_PAL_OK)
      return rc;
    service->data_uplink_task = NULL;
  }
  if (service->data_downlink_task != NULL) {
    rc = h2_pal_task_join(service->config.task, service->data_downlink_task);
    if (rc != H2_PAL_OK)
      return rc;
    service->data_downlink_task = NULL;
  }
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
  if (!service->stopped || service->dispatching ||
      service->active_count != 0u || service->caller_reference_count != 0u ||
      service->request_reference_count != 0u || service->pcm_track_refs != 0u ||
      service->pcm_track_unsetting || service->queued_event_count != 0u ||
      service->dispatch_item_count != 0u ||
      (service->terminal_pending && !service->terminal_dispatched)) {
    unlock_service(service);
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_track_t *track = atomic_exchange(&service->pcm_track, NULL);
  h2_gizclaw_pcm_track_detach_internal(track);
  unlock_service(service);
  h2_pal_queue_destroy(service->config.queue, service->dispatch_queue);
  h2_pal_queue_destroy(service->config.queue, service->request_queue);
  (void)h2_pal_cond_destroy(service->config.sync, service->progress_cond);
  (void)h2_pal_mutex_destroy(service->config.sync, service->mutex);
  (void)h2_pal_mutex_destroy(service->config.sync, service->audio_mutex);
  h2_pal_mem_free(service->config.client_config->allocator, service);
  return H2_PAL_OK;
}
