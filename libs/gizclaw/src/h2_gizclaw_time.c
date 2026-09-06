#include "h2_gizclaw_service_internal.h"
#include "h2_yyjson_json.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define TIME_RETRY_MS 30000u

static int time_canceled(void *user) {
  h2_gizclaw_service_t *service = user;
  return service->client_config.cancel_requested(
      service->client_config.cancel_user);
}

static h2_pal_result_t parse_time(h2_gizclaw_service_t *service,
                                  const uint8_t *body, size_t size,
                                  uint64_t *out_ms) {
  h2_yyjson_json_t *provider = NULL;
  h2_pal_result_t rc = h2_yyjson_json_create(
      service->client_config.allocator, &provider);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_json_api_t *json = h2_yyjson_json_api(provider);
  h2_pal_json_document_t *doc = NULL;
  h2_pal_json_value_t *root = NULL, *value = NULL;
  const h2_pal_json_limits_t limits = {
      .max_document_bytes = 16384u, .max_depth = 16u, .max_values = 1024u};
  rc = h2_pal_json_document_parse(json, body, size, &limits, &doc);
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_document_root(json, doc, &root);
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_object_get(json, root, "server_time", 11u, &value);
  double number = 0;
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_value_get_number(json, value, &number);
  if (rc == H2_PAL_OK) {
    /* Epoch milliseconds must be positive, integral and exactly representable.
     * No timezone offset is applied here. */
    if (!(number > 0 && number <= 9007199254740991.0) ||
        (double)(uint64_t)number != number)
      rc = H2_PAL_ERR_FORMAT;
    else
      *out_ms = (uint64_t)number;
  }
  (void)h2_pal_json_document_destroy(json, &doc);
  (void)h2_yyjson_json_destroy(&provider);
  return rc;
}

static h2_pal_result_t sync_time(h2_gizclaw_service_t *service) {
  const h2_gizclaw_config_t *config = &service->client_config;
  if (config->http == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  const h2_gizclaw_str_t endpoint = config->server_endpoint;
  if (endpoint.data == NULL || endpoint.len == 0u || endpoint.len > INT_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  char url[512];
  int length = snprintf(url, sizeof(url), "http://%.*s/server-info",
                        (int)endpoint.len, endpoint.data);
  if (length <= 0 || (size_t)length >= sizeof(url))
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t *body = h2_pal_mem_alloc(config->allocator, 16384u);
  if (body == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_GET,
      .url = {.data = url, .len = (size_t)length},
      .timeout_ms = 5000u,
      .response_buf = body,
      .response_buf_cap = 16384u,
      .allocator = config->allocator,
      .response_allocator = config->allocator,
      .cancel_cb = time_canceled,
      .cancel_user = service,
  };
  h2_pal_http_response_t response;
  h2_pal_http_response_reset(&response);
  h2_pal_result_t rc = h2_pal_http_request(config->http, &request, &response);
  if (rc == H2_PAL_OK &&
      (response.status_code < 200 || response.status_code >= 300))
    rc = H2_PAL_ERR_IO;
  uint64_t wall_ms = 0u;
  if (rc == H2_PAL_OK)
    rc = parse_time(service, response.body, response.body_len, &wall_ms);
  if (rc == H2_PAL_OK)
    rc = time_canceled(service) ? H2_PAL_ERR_CLOSED
                               : h2_pal_time_set_wall_ms(config->time, wall_ms);
  h2_pal_http_response_free(config->http, &response);
  h2_pal_mem_free(config->allocator, body);
  return rc;
}

static void time_worker(void *user) {
  h2_gizclaw_service_t *service = user;
  for (;;) {
    if (time_canceled(service))
      return;
    if (h2_pal_mutex_lock(service->config.sync, service->mutex) != H2_PAL_OK)
      return;
    service->time_sync.state = H2_GIZCLAW_TIME_SYNC_RUNNING;
    ++service->time_sync.attempts;
    (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
    h2_pal_result_t rc = sync_time(service);
    if (h2_pal_mutex_lock(service->config.sync, service->mutex) != H2_PAL_OK)
      return;
    service->time_sync.last_result = rc;
    service->time_sync.state = rc == H2_PAL_OK
                                  ? H2_GIZCLAW_TIME_SYNC_SUCCEEDED
                                  : H2_GIZCLAW_TIME_SYNC_RETRY;
    (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
    h2_gizclaw_service_log_request(service,
        rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_WARN,
        "time", rc == H2_PAL_OK ? "calibrated" : "retry", 0u, rc, 0, 0u, 0u);
    if (rc == H2_PAL_OK)
      return;
    uint64_t now = 0u;
    if (h2_pal_time_get_monotonic_ms(service->client_config.time, &now) != H2_PAL_OK)
      return;
    const uint64_t deadline = now + TIME_RETRY_MS;
    if (h2_pal_mutex_lock(service->config.sync, service->mutex) != H2_PAL_OK)
      return;
    while (!service->stopping && !h2_pal_time_deadline_expired(now, deadline)) {
      rc = h2_pal_cond_wait(service->config.sync, service->progress_cond,
                            service->mutex, (uint32_t)(deadline - now));
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT)
        break;
      if (h2_pal_time_get_monotonic_ms(service->client_config.time, &now) != H2_PAL_OK)
        break;
    }
    (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT)
      return;
  }
}

void h2_gizclaw_time_sync_start_internal(h2_gizclaw_service_t *service) {
  if (service->time_task != NULL)
    return;
  uint64_t now = 0u;
  if (h2_pal_time_get_monotonic_ms(service->client_config.time, &now) != H2_PAL_OK ||
      (service->time_start_retry_ms != 0u &&
       !h2_pal_time_deadline_expired(now, service->time_start_retry_ms)))
    return;
  service->time_start_retry_ms = now + TIME_RETRY_MS;
  h2_pal_result_t rc = H2_PAL_ERR_UNSUPPORTED;
  if (service->client_config.http != NULL &&
      service->client_config.server_endpoint.data != NULL &&
      service->client_config.server_endpoint.len != 0u) {
    const h2_pal_task_options_t options = {
        .name = "$gizclaw/time", .min_stack_size = 16384u};
    rc = h2_pal_task_start(service->config.task, &options, time_worker, service,
                           &service->time_task);
  }
  if (rc != H2_PAL_OK &&
      h2_pal_mutex_lock(service->config.sync, service->mutex) == H2_PAL_OK) {
    service->time_sync.state = H2_GIZCLAW_TIME_SYNC_RETRY;
    service->time_sync.last_result = rc;
    (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  }
}

h2_pal_result_t h2_gizclaw_service_get_time_sync_status(
    h2_gizclaw_service_t *service, h2_gizclaw_time_sync_status_t *out_status) {
  if (service == NULL || out_status == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_status, 0, sizeof(*out_status));
  h2_pal_result_t rc = h2_pal_mutex_lock(service->config.sync, service->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  *out_status = service->time_sync;
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  return H2_PAL_OK;
}
