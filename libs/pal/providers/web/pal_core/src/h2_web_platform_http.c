#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define H2_WEB_HTTP_HEADER_FIELDS 4u

EM_ASYNC_JS(int, h2_web_http_fetch_js,
            (int method, const char *url, size_t url_len,
             const uint32_t *header_fields, size_t header_count,
             const uint8_t *body, size_t body_len, int timeout_ms,
             int *out_status, uint32_t *out_headers,
             uint32_t *out_headers_len, uint32_t *out_body,
             uint32_t *out_body_len), {
  const methods = [null, 'GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'HEAD',
                   'OPTIONS'];
  if (method < 1 || method >= methods.length) return -1;
  const controller = new AbortController();
  const timeout = timeout_ms > 0
      ? setTimeout(() => controller.abort(), timeout_ms)
      : 0;
  try {
    const headers = new Headers();
    const base = header_fields >>> 2;
    for (let index = 0; index < header_count; ++index) {
      const field = base + index * 4;
      const name = UTF8ToString(HEAPU32[field], HEAPU32[field + 1]);
      const value = UTF8ToString(HEAPU32[field + 2], HEAPU32[field + 3]);
      headers.append(name, value);
    }
    const requestBody = body_len
        ? HEAPU8.slice(body, body + body_len)
        : undefined;
    const requestUrl = UTF8ToString(url, url_len);
    const proxyUrl = Module['h2WebHttpProxyUrl'];
    const fetchUrl = typeof proxyUrl === 'string' && proxyUrl.length
        ? proxyUrl + encodeURIComponent(requestUrl)
        : requestUrl;
    const response = await fetch(fetchUrl, {
      method: methods[method],
      headers,
      body: method === 1 || method === 6 ? undefined : requestBody,
      credentials: 'omit',
      mode: 'cors',
      redirect: 'follow',
      signal: controller.signal,
    });
    const encoder = new TextEncoder();
    const encodedHeaders = [];
    let headersLength = 4;
    response.headers.forEach((value, name) => {
      const encodedName = encoder.encode(name);
      const encodedValue = encoder.encode(value);
      encodedHeaders.push([encodedName, encodedValue]);
      headersLength += 8 + encodedName.byteLength + encodedValue.byteLength;
    });
    // Finish asynchronous body reads before owning Wasm allocations: rejection
    // or timeout must not leak headers on each failed attempt.
    const bytes = new Uint8Array(await response.arrayBuffer());
    const responseHeaders = _malloc(headersLength);
    if (!responseHeaders) return -5;
    const headerView = new DataView(HEAPU8.buffer, responseHeaders,
                                    headersLength);
    headerView.setUint32(0, encodedHeaders.length, true);
    let headerOffset = 4;
    for (const [name, value] of encodedHeaders) {
      headerView.setUint32(headerOffset, name.byteLength, true);
      headerView.setUint32(headerOffset + 4, value.byteLength, true);
      headerOffset += 8;
      HEAPU8.set(name, responseHeaders + headerOffset);
      headerOffset += name.byteLength;
      HEAPU8.set(value, responseHeaders + headerOffset);
      headerOffset += value.byteLength;
    }
    const responseBody = bytes.byteLength ? _malloc(bytes.byteLength) : 0;
    if (bytes.byteLength && !responseBody) {
      _free(responseHeaders);
      return -5;
    }
    if (bytes.byteLength) HEAPU8.set(bytes, responseBody);
    HEAP32[out_status >>> 2] = response.status;
    HEAPU32[out_headers >>> 2] = responseHeaders;
    HEAPU32[out_headers_len >>> 2] = headersLength;
    HEAPU32[out_body >>> 2] = responseBody;
    HEAPU32[out_body_len >>> 2] = bytes.byteLength;
    return 0;
  } catch (error) {
    const requestUrl = UTF8ToString(url, url_len);
    console.error('Web HTTP request failed', requestUrl, error);
    return error && error.name === 'AbortError' ? -6 : -4;
  } finally {
    if (timeout) clearTimeout(timeout);
  }
});

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

  int status = 0;
  uint32_t fetched_headers = 0u;
  uint32_t fetched_headers_len = 0u;
  uint32_t fetched_body = 0u;
  uint32_t fetched_body_len = 0u;
  int rc = H2_PAL_ERR_IO;
  uint32_t retries_left =
      request->retry_count > 0 ? (uint32_t)request->retry_count : 0u;
  const double start_ms = emscripten_get_now();
  do {
    int attempt_timeout_ms = request->timeout_ms;
    if (request->timeout_ms > 0) {
      const double elapsed_ms = emscripten_get_now() - start_ms;
      if (elapsed_ms >= request->timeout_ms) {
        rc = H2_PAL_ERR_TIMEOUT;
        break;
      }
      attempt_timeout_ms = (int)(request->timeout_ms - elapsed_ms);
      if (attempt_timeout_ms < 1) {
        attempt_timeout_ms = 1;
      }
    }
    rc = h2_web_http_fetch_js(
        (int)request->method, request->url.data, request->url.len,
        header_fields, request->header_count, request->body, request->body_len,
        attempt_timeout_ms, &status, &fetched_headers, &fetched_headers_len,
        &fetched_body, &fetched_body_len);
    const int retryable_status = status == 408 || status == 429 ||
                                 status >= 500;
    const int retryable_result = rc == H2_PAL_ERR_IO ||
                                 rc == H2_PAL_ERR_TIMEOUT;
    if ((rc == H2_PAL_OK && !retryable_status) ||
        h2_pal_http_request_is_canceled(request) ||
        (!retryable_result && !retryable_status) || retries_left == 0u) {
      break;
    }
    free((void *)(uintptr_t)fetched_headers);
    free((void *)(uintptr_t)fetched_body);
    fetched_headers = 0u;
    fetched_headers_len = 0u;
    fetched_body = 0u;
    fetched_body_len = 0u;
    status = 0;
    retries_left -= 1u;
  } while (true);
  free(header_fields);
  if (rc != H2_PAL_OK) {
    free((void *)(uintptr_t)fetched_headers);
    free((void *)(uintptr_t)fetched_body);
    return rc;
  }

  if (h2_pal_http_request_is_canceled(request)) {
    free((void *)(uintptr_t)fetched_headers);
    free((void *)(uintptr_t)fetched_body);
    return H2_PAL_ERR_CLOSED;
  }
  rc = h2_web_http_deliver_headers(
      request, (const uint8_t *)(uintptr_t)fetched_headers,
      fetched_headers_len);
  free((void *)(uintptr_t)fetched_headers);
  if (rc != H2_PAL_OK) {
    free((void *)(uintptr_t)fetched_body);
    return rc;
  }
  if (h2_pal_http_request_is_canceled(request)) {
    free((void *)(uintptr_t)fetched_body);
    return H2_PAL_ERR_CLOSED;
  }

  const uint8_t *source = (const uint8_t *)(uintptr_t)fetched_body;
  if (request->read_cb != NULL) {
    size_t offset = 0u;
    while (offset < fetched_body_len) {
      if (h2_pal_http_request_is_canceled(request)) {
        free((void *)(uintptr_t)fetched_body);
        return H2_PAL_ERR_CLOSED;
      }
      size_t chunk_len = fetched_body_len - offset;
      const uint8_t *chunk = source + offset;
      if (request->chunk_buf != NULL && request->chunk_buf_cap != 0u) {
        if (chunk_len > request->chunk_buf_cap) {
          chunk_len = request->chunk_buf_cap;
        }
        memcpy(request->chunk_buf, chunk, chunk_len);
        chunk = request->chunk_buf;
      }
      const size_t total_read = offset + chunk_len;
      const int read_rc = request->read_cb(
          request->user, request, chunk, chunk_len, total_read,
          fetched_body_len - total_read);
      if (read_rc != H2_PAL_OK) {
        free((void *)(uintptr_t)fetched_body);
        return read_rc;
      }
      offset = total_read;
    }
    free((void *)(uintptr_t)fetched_body);
  } else if (request->response_buf != NULL) {
    if ((size_t)fetched_body_len > request->response_buf_cap) {
      free((void *)(uintptr_t)fetched_body);
      return H2_PAL_ERR_NO_MEMORY;
    }
    if (fetched_body_len != 0u) {
      memcpy(request->response_buf, source, fetched_body_len);
    }
    free((void *)(uintptr_t)fetched_body);
    out_response->body = request->response_buf;
    out_response->body_len = fetched_body_len;
  } else {
    const h2_pal_mem_api_t *allocator =
        h2_pal_http_response_allocator(request);
    if (allocator == NULL) {
      free((void *)(uintptr_t)fetched_body);
      return H2_PAL_ERR_INVALID_ARG;
    }
    uint8_t *owned = NULL;
    if (fetched_body_len != 0u) {
      owned = h2_pal_mem_alloc(allocator, fetched_body_len);
      if (owned == NULL) {
        free((void *)(uintptr_t)fetched_body);
        return H2_PAL_ERR_NO_MEMORY;
      }
      memcpy(owned, source, fetched_body_len);
    }
    free((void *)(uintptr_t)fetched_body);
    out_response->body = owned;
    out_response->body_len = fetched_body_len;
    out_response->allocator = allocator;
  }
  out_response->status_code = status;
  out_response->content_length = fetched_body_len;
  return H2_PAL_OK;
}

static void h2_web_http_response_free(void *user,
                                      h2_pal_http_response_t *response) {
  (void)user;
  if (response == NULL) {
    return;
  }
  if (response->allocator != NULL && response->body != NULL) {
    h2_pal_mem_free(response->allocator, response->body);
  }
  h2_pal_http_response_reset(response);
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
