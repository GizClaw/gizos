#include "h2_gizclaw_service_internal.h"

#include <stdio.h>
#include <string.h>

/* Boundaries waiting for service_poll. The downlink carries one stream at a
 * time, so queued kinds alternate BEGIN/END and a full queue always holds a
 * whole undelivered pair to drop. */
#define H2_GIZCLAW_DOWNLINK_STREAM_PENDING 8u

struct h2_gizclaw_downlink_streams {
  /* Network task only. */
  bool open;
  h2_gizclaw_downlink_stream_event_t current;
  /* Protected by service->mutex. */
  h2_gizclaw_downlink_stream_event_t pending[H2_GIZCLAW_DOWNLINK_STREAM_PENDING];
  size_t count;
  /* service_poll task only. */
  h2_gizclaw_downlink_stream_event_t dispatch;
};

h2_pal_result_t
h2_gizclaw_downlink_streams_create_internal(h2_gizclaw_service_t *service) {
  if (service->config.on_downlink_stream == NULL)
    return H2_PAL_OK;
  h2_gizclaw_downlink_streams_t *streams = h2_pal_mem_alloc(
      service->config.client_config->allocator, sizeof(*streams));
  if (streams == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(streams, 0, sizeof(*streams));
  service->downlink_streams = streams;
  return H2_PAL_OK;
}

void h2_gizclaw_downlink_streams_destroy_internal(
    h2_gizclaw_service_t *service) {
  /* Observations still queued are dropped: they never gate teardown. */
  h2_pal_mem_free(service->config.client_config->allocator,
                  service->downlink_streams);
  service->downlink_streams = NULL;
}

static void log_stream(const h2_gizclaw_service_t *service, const char *stage,
                       size_t detail) {
  if (service->client_config.log == NULL)
    return;
  char message[96];
  (void)snprintf(message, sizeof(message),
                 "event=downlink_stream stage=%s detail=%zu", stage, detail);
  (void)h2_pal_log_write(service->client_config.log, H2_PAL_LOG_WARN,
                         "gizclaw", message);
}

/* Copy a wire field that must hold a NUL within both its storage and max+1
 * bytes; an absent field is empty. Returns its length, or SIZE_MAX when it
 * does not fit. */
static size_t copy_field(char *out, size_t max, const char *field,
                         size_t field_size) {
  out[0] = '\0';
  if (field == NULL || field_size == 0u)
    return 0u;
  const size_t bound = field_size < max + 1u ? field_size : max + 1u;
  const char *end = memchr(field, '\0', bound);
  if (end == NULL)
    return SIZE_MAX;
  const size_t len = (size_t)(end - field);
  memcpy(out, field, len);
  out[len] = '\0';
  return len;
}

static void queue_event(h2_gizclaw_service_t *service,
                        const h2_gizclaw_downlink_stream_event_t *event) {
  h2_gizclaw_downlink_streams_t *streams = service->downlink_streams;
  if (h2_pal_mutex_lock(service->config.sync, service->mutex) != H2_PAL_OK)
    return;
  bool dropped = false;
  if (streams->count == H2_GIZCLAW_DOWNLINK_STREAM_PENDING) {
    /* The head may be the END of a BEGIN already delivered; the next BEGIN
     * starts the oldest pair still entirely queued. */
    const size_t first =
        streams->pending[0].kind == H2_GIZCLAW_DOWNLINK_STREAM_BEGIN ? 0u : 1u;
    memmove(&streams->pending[first], &streams->pending[first + 2u],
            (streams->count - first - 2u) * sizeof(streams->pending[0]));
    streams->count -= 2u;
    dropped = true;
  }
  streams->pending[streams->count++] = *event;
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  h2_gizclaw_service_wake_dispatch_internal(service);
  if (dropped)
    log_stream(service, "dropped_pair", H2_GIZCLAW_DOWNLINK_STREAM_PENDING);
}

static void end_open(h2_gizclaw_service_t *service, bool interrupted) {
  h2_gizclaw_downlink_streams_t *streams = service->downlink_streams;
  if (!streams->open)
    return;
  streams->open = false;
  h2_gizclaw_downlink_stream_event_t event = streams->current;
  event.kind = H2_GIZCLAW_DOWNLINK_STREAM_END;
  event.interrupted = interrupted;
  queue_event(service, &event);
}

void h2_gizclaw_downlink_stream_boundary_internal(
    h2_gizclaw_service_t *service,
    const h2_gizclaw_downlink_boundary_t *boundary) {
  if (service == NULL || boundary == NULL || service->downlink_streams == NULL)
    return;
  h2_gizclaw_downlink_streams_t *streams = service->downlink_streams;
  h2_gizclaw_downlink_stream_event_t event = {
      .kind = boundary->begin ? H2_GIZCLAW_DOWNLINK_STREAM_BEGIN
                              : H2_GIZCLAW_DOWNLINK_STREAM_END};
  const size_t id_len =
      copy_field(event.stream_id, H2_GIZCLAW_DOWNLINK_STREAM_ID_MAX_BYTES,
                 boundary->stream_id, boundary->stream_id_size);
  const bool id_valid = id_len != 0u && id_len != SIZE_MAX;
  const bool same = id_valid && streams->open &&
                    strcmp(event.stream_id, streams->current.stream_id) == 0;
  if (!boundary->begin) {
    if (same)
      end_open(service, boundary->error);
    return;
  }
  if (same)
    return;
  /* A BOS for another stream, valid or not, means the open one was cut off. */
  end_open(service, true);
  const size_t label_len =
      copy_field(event.label, H2_GIZCLAW_DOWNLINK_STREAM_LABEL_MAX_BYTES,
                 boundary->label, boundary->label_size);
  if (!id_valid || label_len == SIZE_MAX) {
    log_stream(service, id_valid ? "label_rejected" : "stream_id_rejected",
               id_valid ? boundary->label_size : boundary->stream_id_size);
    return;
  }
  streams->current = event;
  streams->open = true;
  queue_event(service, &event);
}

void h2_gizclaw_downlink_stream_closed_internal(
    h2_gizclaw_service_t *service) {
  if (service != NULL && service->downlink_streams != NULL)
    end_open(service, true);
}

bool h2_gizclaw_downlink_stream_ready_locked(
    const h2_gizclaw_service_t *service) {
  return service->downlink_streams != NULL &&
         service->downlink_streams->count != 0u;
}

bool h2_gizclaw_downlink_stream_dispatch_step_internal(
    h2_gizclaw_service_t *service) {
  h2_gizclaw_downlink_streams_t *streams = service->downlink_streams;
  if (streams == NULL ||
      h2_pal_mutex_lock(service->config.sync, service->mutex) != H2_PAL_OK)
    return false;
  const bool ready = streams->count != 0u;
  if (ready) {
    streams->dispatch = streams->pending[0];
    --streams->count;
    memmove(&streams->pending[0], &streams->pending[1],
            streams->count * sizeof(streams->pending[0]));
  }
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  if (ready)
    service->config.on_downlink_stream(service->config.downlink_stream_user,
                                       &streams->dispatch);
  return ready;
}
