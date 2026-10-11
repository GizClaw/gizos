#include "h2_gizclaw_e2e_device_http.h"
#include <stdio.h>
#include <string.h>
typedef struct api_http_job {
  h2_gizclaw_e2e_api_t *test;
  const h2_pal_http_request_t *request;
  h2_pal_http_response_t *response;
} api_http_job_t;

static int api_http_job_run(void *user) {
  api_http_job_t *job = user;
  return h2_pal_http_request(job->test->fixture->http, job->request,
                             job->response);
}

static int api_call_bytes(h2_gizclaw_e2e_api_t *test, int method,
                          const char *path, const uint8_t *body,
                          size_t body_size, const char *content_type,
                          int expected) {
  (void)h2_pal_json_document_destroy(test->json, &test->document);
  test->root = NULL;
  char url[2048], authorization[112];
  int count = snprintf(url, sizeof(url), "%s/gizclaw/v1%s",
                       test->fixture->config->device_api_url, path);
  if (count < 0 || (size_t)count >= sizeof(url))
    return H2_PAL_ERR_INVALID_ARG;
  (void)snprintf(authorization, sizeof(authorization), "Bearer %s",
                 test->key.secret);
  const h2_pal_http_header_t headers[] = {
      {{"Authorization", 13}, {authorization, strlen(authorization)}},
      {{"Content-Type", 12}, {content_type, strlen(content_type)}},
  };
  uint8_t bytes[16384];
  h2_pal_http_request_t request = {
      .method = method,
      .url = {url, strlen(url)},
      .headers = headers,
      .header_count = 2,
      .timeout_ms = test->timeout_ms ? (int)test->timeout_ms : 15000,
      .body = body,
      .body_len = body_size,
      .response_buf = bytes,
      .response_buf_cap = sizeof(bytes),
      .allocator = test->fixture->allocator};
  h2_pal_http_response_t response = {0};
  api_http_job_t job = {
      .test = test, .request = &request, .response = &response};
  int rc = h2_gizclaw_e2e_fixture_call_sync(
      test->fixture, test->fixture->actors[H2_GIZCLAW_E2E_OWNER].service,
      api_http_job_run, &job);
  h2_gizclaw_e2e_emit(
      "H2_GIZCLAW_E2E stage=device-api path=%s http=%d expected=%d rc=%d\n",
      path, response.status_code, expected, rc);
  if (rc == H2_PAL_OK && response.body_len) {
    rc = h2_pal_json_document_parse(test->json, response.body,
                                    response.body_len, NULL, &test->document);
    if (rc == H2_PAL_OK)
      rc = h2_pal_json_document_root(test->json, test->document, &test->root);
  }
  if (rc == H2_PAL_OK && response.status_code != expected) {
    h2_pal_json_string_view_t code = {0};
    if (h2_pal_json_value_get_string(
            test->json, h2_gizclaw_e2e_api_field(test, "error.code"), &code) ==
            H2_PAL_OK &&
        code.len <= 64u) {
      bool safe = true;
      for (size_t i = 0; i < code.len; ++i)
        if (!((code.data[i] >= 'A' && code.data[i] <= 'Z') ||
              (code.data[i] >= '0' && code.data[i] <= '9') ||
              code.data[i] == '_'))
          safe = false;
      if (safe)
        h2_gizclaw_e2e_emit("H2_GIZCLAW_E2E stage=device-api-error code=%.*s\n",
                            (int)code.len, code.data);
    }
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_pal_http_response_free(test->fixture->http, &response);
  memset(authorization, 0, sizeof(authorization));
  return rc;
}
int h2_gizclaw_e2e_api_call(h2_gizclaw_e2e_api_t *test, int method,
                            const char *path, const char *body, int expected) {
  return api_call_bytes(test, method, path, (const uint8_t *)body,
                        body ? strlen(body) : 0, "application/json", expected);
}
int h2_gizclaw_e2e_lua_upload(h2_gizclaw_e2e_api_t *test,
                              const uint8_t *archive, size_t size,
                              const char *sha256, int expected) {
  if (!archive || !size || size > 512u * 1024u || !sha256 ||
      strlen(sha256) != 64u || strspn(sha256, "0123456789abcdefABCDEF") != 64u)
    return H2_PAL_ERR_INVALID_ARG;
  char path[192];
  int n = snprintf(path, sizeof(path),
                   "/device/lua-app/install?content_length=%zu&sha256=%s", size,
                   sha256);
  if (n < 0 || (size_t)n >= sizeof(path))
    return H2_PAL_ERR_NO_SPACE;
  uint32_t saved_timeout = test->timeout_ms;
  test->timeout_ms = 120000u;
  int rc = api_call_bytes(test, H2_PAL_HTTP_POST, path, archive, size,
                          "application/octet-stream", expected);
  test->timeout_ms = saved_timeout;
  return rc;
}
int h2_gizclaw_e2e_tool_call(h2_gizclaw_e2e_api_t *test, const char *tool,
                             const char *args, int expected) {
  size_t capacity = strlen(tool) + (args ? strlen(args) : 2u) + 64u;
  if (capacity > 256u * 1024u + 2048u)
    return H2_PAL_ERR_NO_SPACE;
  char *body = h2_pal_mem_alloc(test->fixture->allocator, capacity);
  if (body == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  int n = snprintf(body, capacity, "{\"tool\":\"%s\",\"args\":%s}", tool,
                   args ? args : "{}");
  uint32_t saved_timeout = test->timeout_ms;
  test->timeout_ms = !strcmp(tool, "lua.app.install") ? 120000u : 15000u;
  int rc =
      n < 0 || (size_t)n >= capacity
          ? H2_PAL_ERR_INVALID_ARG
          : h2_gizclaw_e2e_api_call(test, H2_PAL_HTTP_POST,
                                    "/device/tool/v0/invoke", body, expected);
  test->timeout_ms = saved_timeout;
  h2_pal_mem_free(test->fixture->allocator, body);
  return rc;
}
h2_pal_json_value_t *h2_gizclaw_e2e_api_field(h2_gizclaw_e2e_api_t *test,
                                              const char *path) {
  h2_pal_json_value_t *value = test->root;
  while (value && *path) {
    size_t len = strcspn(path, ".");
    h2_pal_json_value_t *child = NULL;
    if (h2_pal_json_object_get(test->json, value, path, len, &child) !=
        H2_PAL_OK)
      return NULL;
    value = child;
    path += len;
    if (*path == '.')
      ++path;
  }
  return value;
}
bool h2_gizclaw_e2e_api_text_is(h2_gizclaw_e2e_api_t *test, const char *path,
                                const char *expected) {
  h2_pal_json_string_view_t value = {0};
  return h2_pal_json_value_get_string(test->json,
                                      h2_gizclaw_e2e_api_field(test, path),
                                      &value) == H2_PAL_OK &&
         value.len == strlen(expected) &&
         !memcmp(value.data, expected, value.len);
}
bool h2_gizclaw_e2e_api_number_is(h2_gizclaw_e2e_api_t *test, const char *path,
                                  double expected) {
  double value = 0;
  return h2_pal_json_value_get_number(test->json,
                                      h2_gizclaw_e2e_api_field(test, path),
                                      &value) == H2_PAL_OK &&
         value == expected;
}
