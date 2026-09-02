#include "h2_gizclaw_service_internal.h"

static h2_pal_result_t request_valid(const h2_gizclaw_request_t *request) {
  return request != NULL && request->vtable != NULL ? H2_PAL_OK
                                                     : H2_PAL_ERR_INVALID_ARG;
}

h2_pal_result_t h2_gizclaw_request_do(
    h2_gizclaw_request_t *request, h2_gizclaw_request_callback_fn callback) {
  if (request_valid(request) != H2_PAL_OK ||
      request->vtable->do_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return request->vtable->do_request(request, callback);
}

h2_pal_result_t
h2_gizclaw_request_finish_input(h2_gizclaw_request_t *request) {
  if (request_valid(request) != H2_PAL_OK ||
      request->vtable->finish_input == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return request->vtable->finish_input(request);
}

h2_pal_result_t h2_gizclaw_request_wait(h2_gizclaw_request_t *request,
                                        uint32_t timeout_ms) {
  if (request_valid(request) != H2_PAL_OK || request->vtable->wait == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return request->vtable->wait(request, timeout_ms);
}

h2_pal_result_t h2_gizclaw_request_cancel(h2_gizclaw_request_t *request) {
  if (request_valid(request) != H2_PAL_OK || request->vtable->cancel == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return request->vtable->cancel(request);
}

void h2_gizclaw_request_release(h2_gizclaw_request_t *request) {
  if (request_valid(request) == H2_PAL_OK && request->vtable->release != NULL)
    request->vtable->release(request);
}
