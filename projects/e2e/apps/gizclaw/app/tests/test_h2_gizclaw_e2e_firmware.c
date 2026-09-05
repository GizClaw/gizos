#include "h2_gizclaw_e2e_firmware.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Run the production firmware case with API/HTTP boundary doubles, not a
 * server. The real SHA-256 implementation must verify the downloaded bytes. */
static unsigned mode, live, releases, cancels, http_calls, response_frees;
static unsigned budgets, creates, rpcs, assertions;
static int request_token, service_token;
static void *allocate(void *user, size_t len) {
  (void)user;
  if (mode == 9)
    return NULL;
  void *p = malloc(len);
  assert(p != NULL);
  ++live;
  return p;
}
static void release(void *user, void *p) {
  (void)user;
  assert(live > 0);
  --live;
  free(p);
}
static const h2_pal_mem_vtable_t mem_vt = {.alloc = allocate, .free = release};
static const h2_pal_mem_api_t mem = {.vtable = &mem_vt};
static int now(void *user, uint64_t *out) {
  (void)user;
  *out = 100;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_vt = {.get_monotonic_ms = now};
static const h2_pal_time_api_t clock_api = {.vtable = &time_vt};
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f,
                                     uint32_t ms) {
  assert(f != NULL && ms == 15000);
  ++budgets;
  return mode != 10 && !(mode == 11 && budgets == 2);
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  assert(symbol != NULL && stage != NULL);
  if (strcmp(stage, "firmware_get-assert") == 0 && rc == H2_PAL_OK)
    ++assertions;
}
static void metadata(h2_gizclaw_firmware_t *out, bool rpc) {
  memset(out, 0, sizeof(*out));
  out->channel = H2_GIZCLAW_FIRMWARE_CHANNEL_DEVELOP;
  out->size = 3;
  strcpy(out->url, "https://example.invalid/firmware");
  strcpy(out->sha256,
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  if (!rpc && mode == 1)
    out->channel = H2_GIZCLAW_FIRMWARE_CHANNEL_STABLE;
  if (!rpc && mode == 2)
    out->sha256[0] = 'x';
  if (rpc && mode == 3)
    strcpy(out->url, "http://example.invalid/firmware");
  if (rpc && mode == 19)
    memset(out->url + 8, 'x', sizeof(out->url) - 8);
}
h2_pal_result_t h2_gizclaw_req_create_firmware_get(h2_gizclaw_service_t *s,
                                                   uint64_t id, int32_t channel,
                                                   uint32_t timeout,
                                                   h2_gizclaw_req_t **out) {
  assert(s == (h2_gizclaw_service_t *)&service_token && id == 1 &&
         channel == H2_GIZCLAW_FIRMWARE_CHANNEL_DEVELOP && timeout == 15000);
  ++creates;
  *out = mode == 12 ? NULL : (h2_gizclaw_req_t *)&request_token;
  return mode == 12 ? H2_PAL_ERR_NO_MEMORY : H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *r,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(r == (h2_gizclaw_req_t *)&request_token && !user && !input_read &&
         !output_write);
  return mode == 13 ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *r, uint32_t timeout) {
  assert(r == (h2_gizclaw_req_t *)&request_token && timeout == 15000);
  return mode == 14 ? H2_PAL_ERR_TIMEOUT : H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_resp_parse_firmware_get(const h2_gizclaw_req_t *r,
                                                   h2_gizclaw_firmware_t *out) {
  assert(r == (h2_gizclaw_req_t *)&request_token);
  metadata(out, false);
  return mode == 15 ? H2_PAL_ERR_FORMAT : H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *r) {
  assert(r == (h2_gizclaw_req_t *)&request_token);
  ++cancels;
  return H2_PAL_OK;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *r) {
  if (r != NULL) {
    assert(r == (h2_gizclaw_req_t *)&request_token);
    ++releases;
  }
}
h2_pal_result_t h2_gizclaw_rpc_firmware_get(h2_gizclaw_service_t *s,
                                            int32_t channel, uint32_t timeout,
                                            h2_gizclaw_firmware_t *out) {
  assert(s == (h2_gizclaw_service_t *)&service_token && channel == 3 &&
         timeout == 15000);
  ++rpcs;
  metadata(out, true);
  return mode == 16 ? H2_GIZCLAW_ERR_REMOTE : H2_PAL_OK;
}
static int http_request(void *user, const h2_pal_http_request_t *req,
                        h2_pal_http_response_t *response) {
  (void)user;
  ++http_calls;
  assert(req->method == H2_PAL_HTTP_GET && req->retry_count == 0);
  assert(strcmp(req->url.data, "https://example.invalid/firmware") == 0);
  if (h2_pal_http_request_is_canceled(req))
    return H2_PAL_ERR_CLOSED;
  if (mode == 18)
    return H2_PAL_ERR_IO;
  response->status_code = mode == 4 ? 404 : 200;
  response->content_length = mode == 17 ? 4 : 3;
  const uint8_t *body = (const uint8_t *)(mode == 5 ? "abd" : "abcd");
  const size_t len = mode == 6 ? 2 : mode == 7 ? 4 : 3;
  int rc = req->read_cb(req->user, req, body, 1, 1, len - 1);
  return rc == H2_PAL_OK
             ? req->read_cb(req->user, req, body + 1, len - 1, len, 0)
             : rc;
}
static void http_free(void *user, h2_pal_http_response_t *response) {
  (void)user;
  (void)response;
  ++response_frees;
}
static const h2_pal_http_vtable_t http_vt = {.request = http_request,
                                             .response_free = http_free};
static const h2_pal_http_api_t http = {.vtable = &http_vt};
int main(void) {
  for (mode = 0; mode < 20; ++mode) {
    live = releases = cancels = http_calls = response_frees = 0;
    budgets = creates = rpcs = assertions = 0;
    h2_gizclaw_e2e_config_t config = {0};
    h2_gizclaw_e2e_fixture_t fixture = {.config = &config,
                                        .allocator = &mem,
                                        .http = &http,
                                        .time = &clock_api,
                                        .deadline_ms = 100000,
                                        .cancel_requested = mode == 8};
    fixture.actors[H2_GIZCLAW_E2E_OWNER].service =
        (h2_gizclaw_service_t *)&service_token;
    int rc = h2_gizclaw_e2e_run_firmware(&fixture);
    assert((rc == H2_PAL_OK) == (mode == 0));
    assert(live == 0 && response_frees == http_calls);
    assert(releases == (mode == 10 || mode == 12 ? 0u : 1u));
    assert(cancels == (mode >= 13 && mode <= 15 ? 1u : 0u));
    if (mode == 0)
      assert(creates == 1 && rpcs == 1 && assertions == 2 && http_calls == 1);
    if (mode <= 3 && mode != 0)
      assert(http_calls == 0);
  }
  assert(h2_gizclaw_e2e_run_firmware(NULL) == H2_PAL_ERR_INVALID_ARG);
  return 0;
}
