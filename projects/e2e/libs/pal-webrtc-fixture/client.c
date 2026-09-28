#include "client.h"
#include <stdio.h>
#include <string.h>
static int capture_header(void *user, const h2_pal_http_request_t *request,
                          h2_pal_http_str_t name, h2_pal_http_str_t value) {
  (void)request;
  static const char wanted[] = "x-h2-session-id";
  if (name.len != sizeof(wanted) - 1u)
    return H2_PAL_OK;
  for (size_t i = 0; i < name.len; ++i) {
    char c = name.data[i];
    if (c >= 'A' && c <= 'Z')
      c = (char)(c + ('a' - 'A'));
    if (c != wanted[i])
      return H2_PAL_OK;
  }
  h2_webrtc_fixture_client_t *client = user;
  if (!value.len || value.len >= sizeof(client->session))
    return H2_PAL_ERR_NO_SPACE;
  for (size_t i = 0; i < value.len; ++i)
    if (!((value.data[i] >= 'a' && value.data[i] <= 'z') ||
          (value.data[i] >= '0' && value.data[i] <= '9') ||
          value.data[i] == '-'))
      return H2_PAL_ERR_FORMAT;
  memcpy(client->session, value.data, value.len);
  client->session[value.len] = '\0';
  return H2_PAL_OK;
}
int h2_webrtc_fixture_exchange(void *user, h2_pal_webrtc_str_t offer,
                               char *answer, size_t capacity, size_t *out_len) {
  h2_webrtc_fixture_client_t *client = user;
  if (!client || !client->offer_url || !answer || capacity < 2u || !out_len)
    return H2_PAL_ERR_INVALID_ARG;
  *out_len = 0u;
  client->session[0] = '\0';
  const h2_pal_http_header_t header = {{"Content-Type", 12u},
                                       {"application/sdp", 15u}};
  const h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_POST,
      .url = {client->offer_url, strlen(client->offer_url)},
      .headers = &header,
      .header_count = 1u,
      .body = (const uint8_t *)offer.data,
      .body_len = offer.len,
      .response_buf = (uint8_t *)answer,
      .response_buf_cap = capacity - 1u,
      .timeout_ms = 20000,
      .response_header_cb = capture_header,
      .response_header_user = client,
  };
  h2_pal_http_response_t response = {0};
  int rc = h2_pal_http_do(client->http, &request, &response);
  if (rc == H2_PAL_OK && (response.status_code != 200 || !client->session[0]))
    rc = H2_PAL_ERR_IO;
  if (rc == H2_PAL_OK) {
    *out_len = response.body_len;
    answer[*out_len] = '\0';
  }
  h2_pal_http_response_free(client->http, &response);
  return rc;
}

int h2_webrtc_fixture_close(void *user) {
  h2_webrtc_fixture_client_t *client = user;
  if (!client || !client->session[0])
    return H2_PAL_ERR_INVALID_STATE;
  size_t len = strlen(client->offer_url);
  if (len < 6u || strcmp(client->offer_url + len - 6u, "/offer"))
    return H2_PAL_ERR_INVALID_ARG;
  char url[512];
  int n = snprintf(url, sizeof(url), "%.*s/session/%s/close", (int)(len - 6u),
                   client->offer_url, client->session);
  if (n < 0 || (size_t)n >= sizeof(url))
    return H2_PAL_ERR_NO_SPACE;
  const h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_POST, .url = {url, (size_t)n}, .timeout_ms = 10000};
  h2_pal_http_response_t response = {0};
  int rc = h2_pal_http_do(client->http, &request, &response);
  if (rc == H2_PAL_OK && response.status_code != 204)
    rc = H2_PAL_ERR_IO;
  h2_pal_http_response_free(client->http, &response);
  return rc;
}
