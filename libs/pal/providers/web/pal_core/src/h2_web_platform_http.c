#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_WEB_HTTP_HEADER_FIELDS 4u
// Matches the CoreHTTP provider when a request leaves timeout_ms unset.
#define H2_WEB_HTTP_DEFAULT_TIMEOUT_MS 10000
// Cancel callbacks are polled at this interval while a browser Promise waits.
#define H2_WEB_HTTP_CANCEL_POLL_MS 50u

/*
 * Each request is one browser fetch owned by Module.h2WebHttp, keyed by the
 * platform address and a request id. JS never writes into C memory from a
 * Promise callback: completions only record their state and wake the waiting
 * task through h2_web_async_complete(); C then copies what it needs with
 * synchronous calls. The response body streams chunk by chunk.
 */

// clang-format off
EM_JS(int, h2_web_http_start_js,
      (uintptr_t platform_address, uint32_t request_id, uint32_t op_id,
       int method, const char *url, size_t url_len,
       const uint32_t *header_fields, size_t header_count,
       const uint8_t *body, size_t body_len, int timeout_ms), {
  const methods = [null, 'GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'HEAD',
                   'OPTIONS'];
  if (method < 1 || method >= methods.length) return -1;
  if (typeof fetch !== 'function' || typeof AbortController !== 'function') {
    return -3;
  }
  const platforms = Module['h2WebHttp'] ||= new Map();
  let requests = platforms.get(platform_address);
  if (!requests) {
    requests = new Map();
    platforms.set(platform_address, requests);
  }
  const requestUrl = UTF8ToString(url, url_len);
  const entry = {
    op: op_id,
    controller: new AbortController(),
    status: 0,
    headers: [],
    contentLength: -1,
    response: null,
    reader: null,
    chunk: null,
    offset: 0,
    eof: false,
    error: 0,
    timedOut: false,
    canceled: false,
    // Diagnostics never include the query string, which may carry tokens.
    target: (() => {
      try {
        const parsed = new URL(requestUrl, globalThis.location?.href);
        return parsed.origin + parsed.pathname;
      } catch (_) {
        return '<invalid URL>';
      }
    })(),
  };
  const complete = (result) => {
    if (requests.get(request_id) !== entry) return;
    Module['_h2_web_async_complete'](platform_address, entry.op, result);
  };
  entry.fail = (error) => {
    if (entry.error) return entry.error;
    if (entry.canceled) {
      entry.error = -10;
    } else if (entry.timedOut) {
      entry.error = -6;
    } else if (globalThis.navigator && navigator.onLine === false) {
      entry.error = -2;
    } else {
      entry.error = -4;
    }
    const reason = entry.error === -6 ? 'timed out'
        : entry.error === -10 ? 'canceled'
        : entry.error === -2 ? 'browser is offline'
        : 'network, DNS, TLS, mixed-content, CSP or CORS failure; the ' +
          'browser console names the exact cause and CORS needs ' +
          'Access-Control-Allow-Origin on every response';
    if (entry.error !== -10) {
      console.error(`Web HTTP ${methods[method]} ${entry.target} failed: ` +
                    `${reason} (${error?.name || 'Error'}: ` +
                    `${error?.message || error})`);
    }
    return entry.error;
  };
  entry.timer = setTimeout(() => {
    entry.timedOut = true;
    entry.controller.abort();
  }, Math.max(1, timeout_ms));
  requests.set(request_id, entry);

  const headers = new Headers();
  try {
    const base = header_fields >>> 2;
    for (let index = 0; index < header_count; ++index) {
      const field = base + index * 4;
      headers.append(UTF8ToString(HEAPU32[field], HEAPU32[field + 1]),
                     UTF8ToString(HEAPU32[field + 2], HEAPU32[field + 3]));
    }
  } catch (_) {
    clearTimeout(entry.timer);
    requests.delete(request_id);
    return -1;
  }
  const requestBody = body_len && method !== 1 && method !== 6
      ? HEAPU8.slice(body, body + body_len)
      : undefined;
  const proxyUrl = Module['h2WebHttpProxyUrl'];
  const fetchUrl = typeof proxyUrl === 'string' && proxyUrl.length
      ? proxyUrl + encodeURIComponent(requestUrl)
      : requestUrl;
  fetch(fetchUrl, {
    method: methods[method],
    headers,
    body: requestBody,
    credentials: 'omit',
    mode: 'cors',
    redirect: 'follow',
    signal: entry.controller.signal,
  }).then((response) => {
    entry.response = response;
    entry.status = response.status;
    const encoder = new TextEncoder();
    response.headers.forEach((value, name) => {
      entry.headers.push([encoder.encode(name), encoder.encode(value)]);
    });
    // With Content-Encoding the header counts compressed bytes while the
    // body is decoded, so the length is unknown.
    const length = Number(response.headers.get('content-length'));
    if (!response.headers.has('content-encoding') &&
        Number.isSafeInteger(length) && length >= 0) {
      entry.contentLength = length;
    }
    complete(0);
  }, (error) => complete(entry.fail(error)));
  return 0;
});

EM_JS(int, h2_web_http_status_js,
      (uintptr_t platform_address, uint32_t request_id, int *out_status,
       uint32_t *out_headers_len, double *out_content_length), {
  const entry = Module['h2WebHttp']?.get(platform_address)?.get(request_id);
  if (!entry) return -10;
  if (entry.error) return entry.error;
  let length = 4;
  for (const [name, value] of entry.headers) {
    length += 8 + name.byteLength + value.byteLength;
  }
  HEAP32[out_status >>> 2] = entry.status;
  HEAPU32[out_headers_len >>> 2] = length;
  HEAPF64[out_content_length >>> 3] = entry.contentLength;
  return 0;
});

EM_JS(int, h2_web_http_headers_js,
      (uintptr_t platform_address, uint32_t request_id, uint8_t *out,
       size_t out_len), {
  const entry = Module['h2WebHttp']?.get(platform_address)?.get(request_id);
  if (!entry) return -10;
  const view = new DataView(HEAPU8.buffer, out, out_len);
  view.setUint32(0, entry.headers.length, true);
  let offset = 4;
  for (const [name, value] of entry.headers) {
    if (offset + 8 + name.byteLength + value.byteLength > out_len) return -4;
    view.setUint32(offset, name.byteLength, true);
    view.setUint32(offset + 4, value.byteLength, true);
    offset += 8;
    HEAPU8.set(name, out + offset);
    offset += name.byteLength;
    HEAPU8.set(value, out + offset);
    offset += value.byteLength;
  }
  return 0;
});

// Returns 1 when bytes or EOF are ready now, 0 when op_id will complete.
EM_JS(int, h2_web_http_read_js,
      (uintptr_t platform_address, uint32_t request_id, uint32_t op_id), {
  const requests = Module['h2WebHttp']?.get(platform_address);
  const entry = requests?.get(request_id);
  if (!entry) return -10;
  if (entry.error) return entry.error;
  if (entry.eof || (entry.chunk && entry.offset < entry.chunk.byteLength)) {
    return 1;
  }
  entry.op = op_id;
  const complete = (result) => {
    if (requests.get(request_id) !== entry) return;
    Module['_h2_web_async_complete'](platform_address, entry.op, result);
  };
  if (entry.drained) {
    entry.eof = true;
    return 1;
  }
  const body = entry.response.body;
  if (!body) {
    // Environments without streaming bodies still deliver bounded chunks.
    entry.response.arrayBuffer().then((buffer) => {
      entry.chunk = new Uint8Array(buffer);
      entry.offset = 0;
      entry.eof = entry.chunk.byteLength === 0;
      entry.drained = true;
      complete(0);
    }, (error) => complete(entry.fail(error)));
    return 0;
  }
  entry.reader ||= body.getReader();
  const next = () => entry.reader.read().then(({done, value}) => {
    if (done) {
      entry.eof = true;
    } else if (!value || value.byteLength === 0) {
      // An empty chunk is not the end of the body.
      next();
      return;
    } else {
      entry.chunk = value;
      entry.offset = 0;
    }
    complete(0);
  }, (error) => complete(entry.fail(error)));
  next();
  return 0;
});

// Copies up to out_cap buffered body bytes; 0 means end of body.
EM_JS(int, h2_web_http_take_js,
      (uintptr_t platform_address, uint32_t request_id, uint8_t *out,
       size_t out_cap), {
  const entry = Module['h2WebHttp']?.get(platform_address)?.get(request_id);
  if (!entry) return -10;
  if (entry.error) return entry.error;
  if (!entry.chunk || entry.offset >= entry.chunk.byteLength) return 0;
  const length = Math.min(out_cap, entry.chunk.byteLength - entry.offset);
  HEAPU8.set(entry.chunk.subarray(entry.offset, entry.offset + length), out);
  entry.offset += length;
  if (entry.offset >= entry.chunk.byteLength) {
    entry.chunk = null;
    if (entry.drained) entry.eof = true;
  }
  return length;
});

EM_JS(size_t, h2_web_http_pending_js,
      (uintptr_t platform_address, uint32_t request_id), {
  const entry = Module['h2WebHttp']?.get(platform_address)?.get(request_id);
  return entry?.chunk ? entry.chunk.byteLength - entry.offset : 0;
});

EM_JS(void, h2_web_http_cancel_js,
      (uintptr_t platform_address, uint32_t request_id), {
  const entry = Module['h2WebHttp']?.get(platform_address)?.get(request_id);
  if (!entry || entry.error) return;
  entry.canceled = true;
  entry.error = -10;
  entry.controller.abort();
});

EM_JS(void, h2_web_http_close_js,
      (uintptr_t platform_address, uint32_t request_id), {
  const requests = Module['h2WebHttp']?.get(platform_address);
  const entry = requests?.get(request_id);
  if (!entry) return;
  requests.delete(request_id);
  if (!requests.size) Module['h2WebHttp'].delete(platform_address);
  clearTimeout(entry.timer);
  if (!entry.eof) {
    entry.canceled = true;
    entry.controller.abort();
  }
  if (entry.reader) entry.reader.cancel().catch(() => {});
});
// clang-format on

static void h2_web_http_response_free_body(h2_pal_http_response_t *response) {
  if (response->allocator != NULL && response->body != NULL) {
    h2_pal_mem_free(response->allocator, response->body);
  }
  h2_pal_http_response_reset(response);
}

typedef struct h2_web_http_exchange {
  h2_web_platform_t *platform;
  const h2_pal_http_request_t *request;
  uint32_t id;
  double deadline_ms;
} h2_web_http_exchange_t;

/* Wait for one browser completion, observing cancel_cb and the deadline. */
static h2_pal_result_t h2_web_http_wait(h2_web_http_exchange_t *exchange,
                                        h2_web_async_t *op) {
  h2_pal_result_t result = H2_PAL_ERR_TIMEOUT;
  for (;;) {
    if (h2_pal_http_request_is_canceled(exchange->request)) {
      h2_web_http_cancel_js((uintptr_t)exchange->platform, exchange->id);
      result = H2_PAL_ERR_CLOSED;
      break;
    }
    const double remaining_ms = exchange->deadline_ms - emscripten_get_now();
    if (remaining_ms < -1000.0) {
      // The JS timer aborts at the deadline; never hang if it cannot report.
      h2_web_http_cancel_js((uintptr_t)exchange->platform, exchange->id);
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    uint32_t slice = H2_WEB_HTTP_CANCEL_POLL_MS;
    if (remaining_ms > 0.0 && remaining_ms < (double)slice)
      slice = (uint32_t)remaining_ms + 1u;
    result = h2_web_async_wait(exchange->platform, op, slice);
    if (result == H2_PAL_OK) {
      result = (h2_pal_result_t)op->result;
      break;
    }
    if (result == H2_PAL_ERR_CLOSED)
      h2_web_http_cancel_js((uintptr_t)exchange->platform, exchange->id);
    if (result != H2_PAL_ERR_TIMEOUT)
      break;
  }
  h2_web_async_end(exchange->platform, op);
  return result;
}

static uint32_t h2_web_http_read_u32(const uint8_t *data) {
  uint32_t value = 0u;
  memcpy(&value, data, sizeof(value));
  return value;
}

static int h2_web_http_deliver_headers(const h2_pal_http_request_t *request,
                                       const uint8_t *data, size_t len) {
  if (data == NULL || len < sizeof(uint32_t)) {
    return H2_PAL_ERR_IO;
  }
  const uint32_t count = h2_web_http_read_u32(data);
  size_t offset = sizeof(uint32_t);
  for (uint32_t index = 0u; index < count; ++index) {
    if (len - offset < 2u * sizeof(uint32_t)) {
      return H2_PAL_ERR_IO;
    }
    const uint32_t name_len = h2_web_http_read_u32(data + offset);
    const uint32_t value_len =
        h2_web_http_read_u32(data + offset + sizeof(uint32_t));
    offset += 2u * sizeof(uint32_t);
    if ((size_t)name_len > len - offset) {
      return H2_PAL_ERR_IO;
    }
    const char *name = (const char *)(data + offset);
    offset += name_len;
    if ((size_t)value_len > len - offset) {
      return H2_PAL_ERR_IO;
    }
    const char *value = (const char *)(data + offset);
    offset += value_len;
    const int rc = h2_pal_http_deliver_response_header(
        request, name, name_len, value, value_len);
    if (rc != H2_PAL_OK) {
      return rc;
    }
  }
  return offset == len ? H2_PAL_OK : H2_PAL_ERR_IO;
}

/* Wait until body bytes or EOF are buffered in JS. */
static h2_pal_result_t h2_web_http_fill(h2_web_http_exchange_t *exchange) {
  h2_web_async_t op;
  h2_web_async_begin(exchange->platform, &op);
  const int ready = h2_web_http_read_js((uintptr_t)exchange->platform,
                                        exchange->id, op.id);
  if (ready != 0) {
    h2_web_async_end(exchange->platform, &op);
    return ready > 0 ? H2_PAL_OK : (h2_pal_result_t)ready;
  }
  return h2_web_http_wait(exchange, &op);
}

typedef struct h2_web_http_body {
  size_t total;
  uint8_t *owned;
  size_t owned_cap;
} h2_web_http_body_t;

static h2_pal_result_t h2_web_http_store(h2_web_http_exchange_t *exchange,
                                         h2_web_http_body_t *body,
                                         double content_length,
                                         h2_pal_http_response_t *response) {
  const h2_pal_http_request_t *request = exchange->request;
  const uintptr_t platform = (uintptr_t)exchange->platform;
  const h2_pal_mem_api_t *allocator = h2_pal_http_response_allocator(request);
  for (;;) {
    // read_cb may cancel; nothing already buffered is delivered after that.
    if (h2_pal_http_request_is_canceled(request)) {
      h2_web_http_cancel_js(platform, exchange->id);
      return H2_PAL_ERR_CLOSED;
    }
    h2_pal_result_t result = h2_web_http_fill(exchange);
    if (result != H2_PAL_OK)
      return result;
    const size_t pending = h2_web_http_pending_js(platform, exchange->id);
    if (pending == 0u)
      return H2_PAL_OK;
    if (request->read_cb != NULL) {
      uint8_t *chunk = request->chunk_buf;
      size_t capacity = request->chunk_buf_cap;
      uint8_t *scratch = NULL;
      if (chunk == NULL || capacity == 0u) {
        scratch = malloc(pending);
        if (scratch == NULL)
          return H2_PAL_ERR_NO_MEMORY;
        chunk = scratch;
        capacity = pending;
      }
      const int copied =
          h2_web_http_take_js(platform, exchange->id, chunk, capacity);
      if (copied < 0) {
        free(scratch);
        return (h2_pal_result_t)copied;
      }
      body->total += (size_t)copied;
      const size_t remaining =
          content_length >= (double)body->total
              ? (size_t)(content_length - (double)body->total)
              : 0u;
      result = (h2_pal_result_t)request->read_cb(
          request->user, request, chunk, (size_t)copied, body->total,
          remaining);
      free(scratch);
      if (result != H2_PAL_OK)
        return result;
      continue;
    }
    if (request->response_buf != NULL) {
      if (pending > request->response_buf_cap - body->total)
        return H2_PAL_ERR_NO_SPACE;
      const int copied = h2_web_http_take_js(
          platform, exchange->id, request->response_buf + body->total,
          pending);
      if (copied < 0)
        return (h2_pal_result_t)copied;
      body->total += (size_t)copied;
      response->body = request->response_buf;
      response->body_len = body->total;
      continue;
    }
    if (allocator == NULL) {
      // Nobody owns the bytes: count and discard them like CoreHTTP.
      uint8_t discard[1024];
      const int copied =
          h2_web_http_take_js(platform, exchange->id, discard, sizeof(discard));
      if (copied < 0)
        return (h2_pal_result_t)copied;
      body->total += (size_t)copied;
      response->body_len = body->total;
      continue;
    }
    if (pending > SIZE_MAX - body->total)
      return H2_PAL_ERR_NO_SPACE;
    if (body->total + pending > body->owned_cap) {
      size_t capacity = body->owned_cap == 0u ? 4096u : body->owned_cap;
      while (capacity < body->total + pending)
        capacity = capacity > SIZE_MAX / 2u ? body->total + pending
                                            : capacity * 2u;
      uint8_t *grown = h2_pal_mem_realloc(allocator, body->owned, capacity);
      if (grown == NULL)
        return H2_PAL_ERR_NO_MEMORY;
      body->owned = grown;
      body->owned_cap = capacity;
    }
    const int copied = h2_web_http_take_js(
        platform, exchange->id, body->owned + body->total, pending);
    if (copied < 0)
      return (h2_pal_result_t)copied;
    body->total += (size_t)copied;
  }
}

typedef enum h2_web_http_attempt {
  H2_WEB_HTTP_ATTEMPT_DONE,
  H2_WEB_HTTP_ATTEMPT_RETRY,
} h2_web_http_attempt_t;

static h2_pal_result_t
h2_web_http_attempt(h2_web_http_exchange_t *exchange,
                    const uint32_t *header_fields, bool may_retry,
                    h2_pal_http_response_t *out_response,
                    h2_web_http_attempt_t *out_next) {
  const h2_pal_http_request_t *request = exchange->request;
  const uintptr_t platform = (uintptr_t)exchange->platform;
  *out_next = H2_WEB_HTTP_ATTEMPT_DONE;
  exchange->id = ++exchange->platform->http_next_id;
  if (exchange->id == 0u)
    exchange->id = ++exchange->platform->http_next_id;
  const double remaining_ms = exchange->deadline_ms - emscripten_get_now();
  if (remaining_ms <= 0.0)
    return H2_PAL_ERR_TIMEOUT;
  h2_web_async_t op;
  h2_web_async_begin(exchange->platform, &op);
  int started = h2_web_http_start_js(
      platform, exchange->id, op.id, (int)request->method, request->url.data,
      request->url.len, header_fields, request->header_count, request->body,
      request->body_len, (int)(remaining_ms + 0.5));
  if (started != 0) {
    h2_web_async_end(exchange->platform, &op);
    return (h2_pal_result_t)started;
  }
  h2_pal_result_t result = h2_web_http_wait(exchange, &op);
  int status = 0;
  uint32_t headers_len = 0u;
  double content_length = -1.0;
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_web_http_status_js(
        platform, exchange->id, &status, &headers_len, &content_length);
  }
  if (result != H2_PAL_OK) {
    h2_web_http_close_js(platform, exchange->id);
    // All attempts share one deadline, so a TIMEOUT ends the request.
    if (may_retry && result == H2_PAL_ERR_IO &&
        emscripten_get_now() < exchange->deadline_ms)
      *out_next = H2_WEB_HTTP_ATTEMPT_RETRY;
    return result;
  }
  if (may_retry && (status == 408 || status == 429 || status >= 500)) {
    h2_web_http_close_js(platform, exchange->id);
    *out_next = H2_WEB_HTTP_ATTEMPT_RETRY;
    return H2_PAL_OK;
  }
  uint8_t *headers = malloc(headers_len);
  if (headers == NULL) {
    h2_web_http_close_js(platform, exchange->id);
    return H2_PAL_ERR_NO_MEMORY;
  }
  result = (h2_pal_result_t)h2_web_http_headers_js(platform, exchange->id,
                                                   headers, headers_len);
  if (result == H2_PAL_OK)
    result = h2_web_http_deliver_headers(request, headers, headers_len);
  free(headers);
  h2_web_http_body_t body = {0};
  if (result == H2_PAL_OK)
    result = h2_web_http_store(exchange, &body, content_length, out_response);
  h2_web_http_close_js(platform, exchange->id);
  if (result != H2_PAL_OK) {
    if (body.owned != NULL)
      h2_pal_mem_free(h2_pal_http_response_allocator(request), body.owned);
    h2_pal_http_response_reset(out_response);
    return result;
  }
  if (body.owned != NULL) {
    out_response->body = body.owned;
    out_response->body_len = body.total;
    out_response->allocator = h2_pal_http_response_allocator(request);
  }
  out_response->status_code = status;
  out_response->content_length =
      content_length >= 0.0 ? (int64_t)content_length : (int64_t)body.total;
  return H2_PAL_OK;
}

static int h2_web_http_request(void *user,
                               const h2_pal_http_request_t *request,
                               h2_pal_http_response_t *out_response) {
  h2_web_platform_t *platform = user;
  h2_pal_http_response_reset(out_response);
  if (platform == NULL || platform->shutting_down || request == NULL ||
      request->url.data == NULL || request->url.len == 0u ||
      (request->header_count != 0u && request->headers == NULL) ||
      request->header_count > SIZE_MAX /
                                  (H2_WEB_HTTP_HEADER_FIELDS *
                                   sizeof(uint32_t)) ||
      (request->body == NULL && request->body_len != 0u) ||
      (request->chunk_buf == NULL && request->chunk_buf_cap != 0u) ||
      (request->response_buf == NULL && request->response_buf_cap != 0u) ||
      request->url.len > UINT32_MAX || request->body_len > UINT32_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (h2_pal_http_request_is_canceled(request)) {
    return H2_PAL_ERR_CLOSED;
  }
  if (request->interface_name != NULL && request->interface_name[0] != '\0') {
    return H2_PAL_ERR_UNSUPPORTED;
  }

  uint32_t *header_fields = NULL;
  if (request->header_count != 0u) {
    header_fields = calloc(request->header_count * H2_WEB_HTTP_HEADER_FIELDS,
                           sizeof(*header_fields));
    if (header_fields == NULL) {
      return H2_PAL_ERR_NO_MEMORY;
    }
    for (size_t index = 0u; index < request->header_count; ++index) {
      const h2_pal_http_header_t *header = &request->headers[index];
      if (header->name.data == NULL || header->name.len == 0u ||
          header->name.len > UINT32_MAX || header->value.len > UINT32_MAX ||
          (header->value.data == NULL && header->value.len != 0u)) {
        free(header_fields);
        return H2_PAL_ERR_INVALID_ARG;
      }
      const size_t field = index * H2_WEB_HTTP_HEADER_FIELDS;
      header_fields[field] = (uint32_t)(uintptr_t)header->name.data;
      header_fields[field + 1u] = (uint32_t)header->name.len;
      header_fields[field + 2u] = (uint32_t)(uintptr_t)header->value.data;
      header_fields[field + 3u] = (uint32_t)header->value.len;
    }
  }

  const int timeout_ms = request->timeout_ms > 0
                             ? request->timeout_ms
                             : H2_WEB_HTTP_DEFAULT_TIMEOUT_MS;
  h2_web_http_exchange_t exchange = {
      .platform = platform,
      .request = request,
      .deadline_ms = emscripten_get_now() + (double)timeout_ms,
  };
  uint32_t retries_left =
      request->retry_count > 0 ? (uint32_t)request->retry_count : 0u;
  ++platform->http_requests;
  h2_pal_result_t result = H2_PAL_ERR_IO;
  for (;;) {
    h2_web_http_attempt_t next = H2_WEB_HTTP_ATTEMPT_DONE;
    result = h2_web_http_attempt(&exchange, header_fields, retries_left != 0u,
                                 out_response, &next);
    if (next != H2_WEB_HTTP_ATTEMPT_RETRY ||
        h2_pal_http_request_is_canceled(request))
      break;
    --retries_left;
  }
  --platform->http_requests;
  free(header_fields);
  if (result == H2_PAL_OK && h2_pal_http_request_is_canceled(request)) {
    h2_web_http_response_free_body(out_response);
    result = H2_PAL_ERR_CLOSED;
  }
  return result;
}

static void h2_web_http_response_free(void *user,
                                      h2_pal_http_response_t *response) {
  (void)user;
  if (response == NULL) {
    return;
  }
  h2_web_http_response_free_body(response);
}

static const h2_pal_http_vtable_t h2_web_http_vtable = {
    .request = h2_web_http_request,
    .response_free = h2_web_http_response_free,
};

void h2_web_platform_http_init(h2_web_platform_t *platform) {
  if (platform == NULL) {
    return;
  }
  platform->http_api.user = platform;
  platform->http_api.vtable = &h2_web_http_vtable;
}

const h2_pal_http_api_t *
h2_web_platform_http_api(h2_web_platform_t *platform) {
  if (platform == NULL) {
    return h2_pal_unsupported_http_api();
  }
  return &platform->http_api;
}
