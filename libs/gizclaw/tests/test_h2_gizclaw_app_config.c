#include "h2_gizclaw_app_config.h"
#include "h2_gizclaw_service_internal.h"
#include "payload/system.pb.h"
#include "rpc.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

/* Replace only the common request boundary: real wire encoding, decoding and
 * response storage are exercised against the SDK's generated descriptors. */
struct fake_request {
  h2_gizclaw_req_t base;
  const void *tag;
  uint8_t input[256];
  size_t input_len;
  h2_gizclaw_rpc_response_t response;
};
static struct fake_request req;
static unsigned creates, dos, releases;
static h2_pal_result_t terminal;
static uint8_t wire[8192];
static h2_gizclaw_rpc_method_t method;
h2_pal_result_t
h2_gizclaw_req_create_rpc_internal(h2_gizclaw_service_t *s, uint64_t identity,
                                   h2_gizclaw_rpc_method_t m, const void *tag,
                                   h2_gizclaw_rpc_bytes_t payload,
                                   uint32_t timeout, h2_gizclaw_req_t **out) {
  assert(s && timeout && identity == 0);
  assert(payload.len <= sizeof(req.input));
  method = m;
  req.tag = tag;
  memcpy(req.input, payload.data, payload.len);
  req.input_len = payload.len;
  *out = &req.base;
  ++creates;
  return H2_PAL_OK;
}
h2_pal_result_t
h2_gizclaw_req_response_internal(const h2_gizclaw_req_t *r, const void *tag,
                                 const h2_gizclaw_rpc_response_t **out) {
  if (!r || ((const struct fake_request *)r)->tag != tag)
    return H2_PAL_ERR_INVALID_ARG;
  if (terminal != H2_PAL_OK)
    return terminal;
  *out = &((const struct fake_request *)r)->response;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_input_internal(const h2_gizclaw_req_t *r,
                                              const void *tag,
                                              h2_gizclaw_rpc_bytes_t *out) {
  assert(r && ((const struct fake_request *)r)->tag == tag);
  *out = (h2_gizclaw_rpc_bytes_t){((const struct fake_request *)r)->input, ((const struct fake_request *)r)->input_len};
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *r, void *user,
                                  h2_gizclaw_req_input_read_fn in,
                                  h2_gizclaw_req_output_write_fn out,
                                  h2_gizclaw_req_complete_fn complete) {
  assert(r && !user && !in && !out && !complete);
  ++dos;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *r, uint32_t timeout) {
  assert(r && timeout == H2_PAL_SYNC_WAIT_FOREVER);
  return terminal;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *r) {
  if (r)
    ++releases;
}
static bool encode_name(pb_ostream_t *s, const pb_field_t *f,
                        void *const *arg) {
  const char *name = *arg;
  return pb_encode_tag_for_field(s, f) &&
         pb_encode_string(s, (const pb_byte_t *)name, strlen(name));
}
static void response(const pb_msgdesc_t *fields, const void *msg) {
  pb_ostream_t s = pb_ostream_from_buffer(wire, sizeof(wire));
  assert(pb_encode(&s, fields, msg));
  req.response = (h2_gizclaw_rpc_response_t){
      .result_payload = wire, .result_payload_len = s.bytes_written};
}
static h2_gizclaw_str_t str(const char *s) {
  return (h2_gizclaw_str_t){s, strlen(s)};
}
int main(void) {
  h2_gizclaw_service_t *service = (h2_gizclaw_service_t *)&creates;
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_app_config_get(service, 0, str("audio.volume"),
                                              100, &request) == H2_PAL_OK);
  assert(method == gizclaw_rpc_v1_RpcMethod_RPC_METHOD_SERVER_APP_CONFIG_GET &&
         dos == 0);
  gizclaw_rpc_v1_AppConfigGetRequest input =
      gizclaw_rpc_v1_AppConfigGetRequest_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(req.input, req.input_len);
  assert(pb_decode(&stream, gizclaw_rpc_v1_AppConfigGetRequest_fields, &input));
  assert(strcmp(input.key, "audio.volume") == 0);
  const char *invalid[] = {"", "A", "a_b", ".a", "a.", "a..b", "a--b", "a-.b"};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    assert(h2_gizclaw_req_create_app_config_get(service, 0, str(invalid[i]),
                                                100, &request) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(!request);
  }
  gizclaw_rpc_v1_AppConfigGetResponse get =
      gizclaw_rpc_v1_AppConfigGetResponse_init_zero;
  get.runtime_profile_name =
      (pb_callback_t){.funcs.encode = encode_name, .arg = "profile"};
  strcpy(get.runtime_profile_revision, "revision");
  memset(get.value, 'x', 4096);
  response(gizclaw_rpc_v1_AppConfigGetResponse_fields, &get);
  uint8_t data[12000];
  h2_gizclaw_resp_storage_t storage = {data, sizeof(data), 7};
  h2_gizclaw_app_config_value_t value;
  assert(h2_gizclaw_rpc_app_config_get(service, str("audio.volume"), 100,
                                       &storage, &value) == H2_PAL_OK);
  assert(value.value.len == 4096 && value.value.data[4096] == 0);
  assert(strcmp(value.runtime_profile_name, "profile") == 0 && releases == 1);
  get.value[0] = 0;
  response(gizclaw_rpc_v1_AppConfigGetResponse_fields, &get);
  storage.used = 0;
  assert(h2_gizclaw_resp_parse_app_config_get(&req.base, &storage, &value) ==
         H2_PAL_OK);
  assert(value.value.len == 0 && value.value.data && !value.value.data[0]);
  /* Append a duplicate value field carrying three opaque bytes, including NUL.
   */
  size_t n = req.response.result_payload_len;
  memcpy(wire + n,
         "\x0a\x03"
         "a\0b",
         5);
  req.response.result_payload_len += 5;
  storage.used = 0;
  assert(h2_gizclaw_resp_parse_app_config_get(&req.base, &storage, &value) ==
         H2_PAL_OK);
  assert(value.value.len == 3 && memcmp(value.value.data, "a\0b", 3) == 0);
  storage.capacity = 1;
  storage.used = 1;
  assert(h2_gizclaw_resp_parse_app_config_get(&req.base, &storage, &value) ==
         H2_PAL_ERR_NO_SPACE);
  assert(storage.used == 1 && value.value.data == NULL);
  storage.capacity = sizeof(data);
  storage.used = 7;
  terminal = H2_PAL_ERR_NOT_FOUND;
  assert(h2_gizclaw_rpc_app_config_get(service, str("a"), 100, &storage,
                                       &value) == H2_PAL_ERR_NOT_FOUND);
  assert(storage.used == 7 && !value.value.data);
  terminal = H2_PAL_OK;
  gizclaw_rpc_v1_AppConfigListResponse list =
      gizclaw_rpc_v1_AppConfigListResponse_init_zero;
  list.runtime_profile_name = get.runtime_profile_name;
  strcpy(list.runtime_profile_revision, "revision");
  list.keys_count = 64;
  for (unsigned i = 0; i < 64; ++i) {
    list.keys[i][0] = 'a' + (char)(i / 26);
    list.keys[i][1] = 'a' + (char)(i % 26);
  }
  response(gizclaw_rpc_v1_AppConfigListResponse_fields, &list);
  h2_gizclaw_app_config_page_t page;
  storage.used = 0;
  assert(h2_gizclaw_rpc_app_config_list(service, (h2_gizclaw_str_t){0}, 64, 100,
                                        &storage, &page) == H2_PAL_OK);
  assert(page.count == 64 && !page.has_next &&
         method == gizclaw_rpc_v1_RpcMethod_RPC_METHOD_SERVER_APP_CONFIG_LIST);
  assert(h2_gizclaw_resp_parse_app_config_get(&req.base, &storage, &value) ==
         H2_PAL_ERR_INVALID_ARG);
  storage.used = 7;
  assert(h2_gizclaw_rpc_app_config_list(service, (h2_gizclaw_str_t){0}, 1, 100,
                                        &storage, &page) == H2_PAL_ERR_FORMAT);
  assert(storage.used == 7 && !page.keys);
  list.keys_count = 0;
  response(gizclaw_rpc_v1_AppConfigListResponse_fields, &list);
  assert(h2_gizclaw_resp_parse_app_config_list(&req.base, &storage, &page) ==
             H2_PAL_OK &&
         page.count == 0);
  list.has_next = true;
  response(gizclaw_rpc_v1_AppConfigListResponse_fields, &list);
  storage.used = 7;
  assert(h2_gizclaw_resp_parse_app_config_list(&req.base, &storage, &page) ==
             H2_PAL_ERR_FORMAT &&
         storage.used == 7);
  wire[0] = 0x80;
  req.response.result_payload_len = 1;
  assert(h2_gizclaw_resp_parse_app_config_list(&req.base, &storage, &page) ==
             H2_PAL_ERR_FORMAT &&
         storage.used == 7);
  return 0;
}
