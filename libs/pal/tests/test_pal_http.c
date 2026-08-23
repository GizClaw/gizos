#include "h2/pal/application/h2_pal_http.h"

#include <assert.h>
#include <string.h>

typedef struct header_capture {
  const h2_pal_http_request_t *request;
  h2_pal_http_str_t name;
  h2_pal_http_str_t value;
  int result;
  size_t calls;
} header_capture_t;

static int capture_header(void *user, const h2_pal_http_request_t *request,
                          h2_pal_http_str_t name, h2_pal_http_str_t value) {
  header_capture_t *capture = user;
  capture->request = request;
  capture->name = name;
  capture->value = value;
  capture->calls++;
  return capture->result;
}

int main(void) {
  header_capture_t capture = {.result = H2_PAL_OK};
  h2_pal_http_request_t request = {
      .response_header_cb = capture_header,
      .response_header_user = &capture,
  };

  assert(h2_pal_http_deliver_response_header(&request, "X-Count", 7u, "42",
                                             2u) == H2_PAL_OK);
  assert(capture.calls == 1u);
  assert(capture.request == &request);
  assert(capture.name.len == 7u &&
         memcmp(capture.name.data, "X-Count", 7u) == 0);
  assert(capture.value.len == 2u && memcmp(capture.value.data, "42", 2u) == 0);

  capture.result = H2_PAL_ERR_CLOSED;
  assert(h2_pal_http_deliver_response_header(&request, "X", 1u, NULL, 0u) ==
         H2_PAL_ERR_CLOSED);
  assert(capture.calls == 2u);

  assert(h2_pal_http_deliver_response_header(NULL, "X", 1u, "", 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_http_deliver_response_header(&request, NULL, 1u, "", 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_http_deliver_response_header(&request, "", 0u, "", 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_http_deliver_response_header(&request, "X\nY", 3u, "", 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_http_deliver_response_header(&request, "X", 1u, "a\r", 2u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(capture.calls == 2u);

  request.response_header_cb = NULL;
  assert(h2_pal_http_deliver_response_header(&request, "X", 1u, "", 0u) ==
         H2_PAL_OK);
  return 0;
}
