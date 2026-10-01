#include "gzc.h"
#include "h2_desktop_platform.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_service_internal.h"
#include "pb_decode.h"
#include "pb_encode.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fake PAL supplies raw reverse-RPC events while the real SDK initializes
 * its transport. It then rejects the first local channel, ending connect before
 * signaling. All request framing, discovery, registration and tool dispatch
 * run through the real SDK and GizOS adapters; no private SDK entry is used. */
typedef struct wire_fixture {
  h2_gizclaw_client_t *client;
  h2_pal_webrtc_event_t event;
  uint8_t sent[8192];
  size_t sent_len;
  unsigned closes, find_calls;
  int remote;
  int64_t value;
  bool exercised;
} wire_fixture_t;
static wire_fixture_t fixture;
static void *allocate(void *user, size_t size) {
  (void)user;
  return malloc(size);
}
static void *resize(void *user, void *p, size_t size) {
  (void)user;
  return realloc(p, size);
}
static void release(void *user, void *p) {
  (void)user;
  free(p);
}
static const h2_pal_mem_vtable_t mem_vtable = {
    .alloc = allocate, .realloc = resize, .free = release};
static const h2_pal_mem_api_t memory = {.vtable = &mem_vtable};
static h2_pal_result_t create_peer(void *user,
                                   const h2_pal_webrtc_peer_config_t *config,
                                   h2_pal_webrtc_peer_t **out) {
  assert(config->allocator == &memory);
  *out = (h2_pal_webrtc_peer_t *)user;
  return H2_PAL_OK;
}
static h2_pal_result_t bind_track(h2_pal_webrtc_peer_t *peer,
                                  h2_pal_webrtc_track_t *track) {
  assert(peer == (h2_pal_webrtc_peer_t *)&fixture && track != NULL);
  return H2_PAL_OK;
}
static h2_pal_result_t poll_peer(h2_pal_webrtc_peer_t *peer, int timeout,
                                 h2_pal_webrtc_event_t *out) {
  (void)timeout;
  wire_fixture_t *f = (wire_fixture_t *)peer;
  if (!f->event.kind)
    return H2_PAL_ERR_WOULD_BLOCK;
  *out = f->event;
  memset(&f->event, 0, sizeof(f->event));
  return H2_PAL_OK;
}
static h2_pal_result_t send_bytes(h2_pal_webrtc_channel_t *channel,
                                  const uint8_t *bytes, size_t length,
                                  int text) {
  assert(channel == (h2_pal_webrtc_channel_t *)&fixture.remote && !text);
  assert(length <= sizeof(fixture.sent) - fixture.sent_len);
  memcpy(fixture.sent + fixture.sent_len, bytes, length);
  fixture.sent_len += length;
  return H2_PAL_OK;
}
static void close_channel(h2_pal_webrtc_channel_t *channel) {
  assert(channel == (h2_pal_webrtc_channel_t *)&fixture.remote);
  ++fixture.closes;
}
static void close_peer(h2_pal_webrtc_peer_t *peer) {
  assert(peer == (h2_pal_webrtc_peer_t *)&fixture);
}
static void emit(wire_fixture_t *f, h2_pal_webrtc_event_t event) {
  f->event = event;
  assert(h2_gizclaw_client_poll(f->client, 0) == H2_PAL_OK);
}
static gzc_rpc_response_t call(wire_fixture_t *f, int method,
                               const uint8_t *bytes, size_t length) {
  const gzc_platform_t *platform =
      gzc_client_platform(h2_gizclaw_client_gzc_internal(f->client));
  gzc_buf_t envelope, frames;
  gzc_buf_init(&envelope);
  gzc_buf_init(&frames);
  assert(gzc_rpc_encode_request_envelope(
             platform, gzc_str_from_cstr("device-test"),
             (gizclaw_rpc_v1_RpcMethod)method,
             gzc_str_from_parts((const char *)bytes, length),
             &envelope) == GZC_OK);
  const gzc_rpc_frame_t binary = {GZC_RPC_FRAME_BINARY, envelope.data,
                                  envelope.len};
  const gzc_rpc_frame_t eos = {GZC_RPC_FRAME_EOS, NULL, 0};
  assert(gzc_rpc_frame_encode(platform, &binary, &frames) == GZC_OK);
  assert(gzc_rpc_frame_encode(platform, &eos, &frames) == GZC_OK);
  const h2_pal_webrtc_channel_info_t info = {
      .label = {"giznet/v1/service/0", 19},
      .stream_id = 7,
      .ordered = 1,
      .reliable = 1};
  f->sent_len = 0;
  unsigned closes = f->closes;
  emit(f,
       (h2_pal_webrtc_event_t){.kind = H2_PAL_WEBRTC_EVENT_CHANNEL_STATE,
                               .peer = (h2_pal_webrtc_peer_t *)f,
                               .channel = (h2_pal_webrtc_channel_t *)&f->remote,
                               .channel_info = info,
                               .channel_state = H2_PAL_WEBRTC_CHANNEL_OPEN});
  emit(f,
       (h2_pal_webrtc_event_t){.kind = H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE,
                               .peer = (h2_pal_webrtc_peer_t *)f,
                               .channel = (h2_pal_webrtc_channel_t *)&f->remote,
                               .channel_info = info,
                               .data = frames.data,
                               .data_len = frames.len});
  if (f->closes != closes + 1 || f->sent_len < 8)
    fprintf(stderr, "method=%d closes=%u->%u sent=%zu\n", method, closes,
            f->closes, f->sent_len);
  assert(f->closes == closes + 1 && f->sent_len >= 8);
  emit(f,
       (h2_pal_webrtc_event_t){.kind = H2_PAL_WEBRTC_EVENT_CHANNEL_STATE,
                               .peer = (h2_pal_webrtc_peer_t *)f,
                               .channel = (h2_pal_webrtc_channel_t *)&f->remote,
                               .channel_info = info,
                               .channel_state = H2_PAL_WEBRTC_CHANNEL_CLOSED});
  size_t first_len = 4u + f->sent[0] + ((size_t)f->sent[1] << 8);
  gzc_rpc_frame_t response_frame;
  assert(gzc_rpc_frame_decode(f->sent, first_len, &response_frame) == GZC_OK);
  assert(response_frame.type == GZC_RPC_FRAME_BINARY);
  assert(f->sent_len == first_len + 4u);
  gzc_rpc_frame_t response_eos;
  assert(gzc_rpc_frame_decode(f->sent + first_len, 4u, &response_eos) ==
         GZC_OK);
  assert(response_eos.type == GZC_RPC_FRAME_EOS);
  gzc_rpc_response_t response = {0};
  assert(gzc_rpc_decode_response_envelope(
             gzc_str_from_parts((const char *)response_frame.data,
                                response_frame.len),
             &response) == GZC_OK);
  gzc_buf_free(&envelope, platform);
  gzc_buf_free(&frames, platform);
  return response;
}
static bool encode_bytes(pb_ostream_t *out, const pb_field_t *field,
                         void *const *arg) {
  const gzc_str_t *bytes = *arg;
  return pb_encode_tag_for_field(out, field) &&
         pb_encode_string(out, (const pb_byte_t *)bytes->data, bytes->len);
}
static bool decode_bytes(pb_istream_t *in, const pb_field_t *field,
                         void **arg) {
  (void)field;
  gzc_str_t *bytes = *arg;
  *bytes = gzc_str_from_parts(in->state, in->bytes_left);
  return pb_read(in, NULL, in->bytes_left);
}
static gzc_rpc_response_t invoke(wire_fixture_t *f, int tool,
                                 gzc_str_t payload) {
  gizclaw_rpc_v1_ClientToolV0InvokeRequest request = {
      .tool = (gizclaw_rpc_v1_ClientTool)tool};
  request.payload.funcs.encode = encode_bytes;
  request.payload.arg = &payload;
  uint8_t bytes[1024];
  pb_ostream_t out = pb_ostream_from_buffer(bytes, sizeof(bytes));
  assert(pb_encode(&out, gizclaw_rpc_v1_ClientToolV0InvokeRequest_fields,
                   &request));
  return call(f, H2_GIZCLAW_RPC_CLIENT_TOOL_V0_INVOKE, bytes,
              out.bytes_written);
}
static int find_tool(void *user, h2_gizclaw_tool_t tool,
                     h2_gizclaw_rpc_bytes_t payload,
                     h2_gizclaw_rpc_provider_response_t *out) {
  wire_fixture_t *f = user;
  assert(tool == H2_GIZCLAW_TOOL_DEVICE_FIND);
  gizclaw_rpc_v1_ClientDeviceFindRequest request = {0};
  pb_istream_t in = pb_istream_from_buffer(payload.data, payload.len);
  assert(
      pb_decode(&in, gizclaw_rpc_v1_ClientDeviceFindRequest_fields, &request));
  assert(request.has_duration_ms && request.duration_ms == 1000);
  ++f->find_calls;
  *out = (h2_gizclaw_rpc_provider_response_t){0};
  return H2_PAL_OK;
}
static h2_pal_result_t read_state(void *user, h2_gizclaw_mhs_read_t *out) {
  out->speaker.has_volume_percent = true;
  out->speaker.volume_percent = (uint32_t)((wire_fixture_t *)user)->value;
  out->speaker.has_muted = true;
  out->speaker.muted = false;
  return H2_PAL_OK;
}
static h2_pal_result_t write_state(void *user,
                                   const h2_gizclaw_mhs_write_t *request,
                                   h2_gizclaw_mhs_read_t *out) {
  wire_fixture_t *fixture = user;
  if (request->speaker.has_volume_percent)
    fixture->value = request->speaker.volume_percent + 1u;
  return read_state(user, out);
}
static void exercise(wire_fixture_t *f) {
  gzc_rpc_response_t response =
      call(f, H2_GIZCLAW_RPC_CLIENT_TOOL_V0_LIST, (const uint8_t *)"", 0);
  assert(!response.has_error);
  gizclaw_rpc_v1_ClientToolV0ListResponse tools = {0};
  pb_istream_t in =
      pb_istream_from_buffer((const pb_byte_t *)response.result_payload.data,
                             response.result_payload.len);
  assert(
      pb_decode(&in, gizclaw_rpc_v1_ClientToolV0ListResponse_fields, &tools));
  const int expected_tools[] = {1, 2, 3, 6};
  assert(tools.tools_count == 4);
  for (size_t i = 0; i < 4; ++i)
    assert((int)tools.tools[i] == expected_tools[i]);
  response =
      call(f, H2_GIZCLAW_RPC_CLIENT_RPC_METHODS_LIST, (const uint8_t *)"", 0);
  assert(!response.has_error);
  gizclaw_rpc_v1_ClientRpcMethodsListResponse methods = {0};
  in = pb_istream_from_buffer((const pb_byte_t *)response.result_payload.data,
                              response.result_payload.len);
  assert(pb_decode(&in, gizclaw_rpc_v1_ClientRpcMethodsListResponse_fields,
                   &methods));
  const int expected_methods[] = {1, 2, 133, 134, 135, 136, 137};
  assert(methods.methods_count == 7);
  for (size_t i = 0; i < 7; ++i)
    assert((int)methods.methods[i] == expected_methods[i]);
  /* A built-in payload passes through SDK tool/v0 response wrapping. */
  response = invoke(f, H2_GIZCLAW_TOOL_INFO_GET, gzc_str_from_cstr(""));
  assert(!response.has_error);
  gzc_str_t inner = {0};
  gizclaw_rpc_v1_ClientToolV0InvokeResponse wrapper = {0};
  wrapper.payload.funcs.decode = decode_bytes;
  wrapper.payload.arg = &inner;
  in = pb_istream_from_buffer((const pb_byte_t *)response.result_payload.data,
                              response.result_payload.len);
  assert(pb_decode(&in, gizclaw_rpc_v1_ClientToolV0InvokeResponse_fields,
                   &wrapper));
  gizclaw_rpc_v1_ClientGetInfoResponse info = {0};
  in = pb_istream_from_buffer((const pb_byte_t *)inner.data, inner.len);
  assert(pb_decode(&in, gizclaw_rpc_v1_ClientGetInfoResponse_fields, &info) &&
         info.has_value);
  const uint8_t find_payload[] = {0x08, 0xe8, 0x07};
  response = invoke(
      f, H2_GIZCLAW_TOOL_DEVICE_FIND,
      gzc_str_from_parts((const char *)find_payload, sizeof(find_payload)));
  assert(!response.has_error && f->find_calls == 1);
  response = invoke(f, H2_GIZCLAW_TOOL_DEVICE_REBOOT, gzc_str_from_cstr(""));
  assert(response.has_error &&
         response.error.code == H2_GIZCLAW_RPC_ERROR_UNIMPLEMENTED);
  const int retired[] = {3, 4, 82, 100, 101, 128, 129, 130, 131, 132};
  for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); ++i) {
    response = call(f, retired[i], (const uint8_t *)"", 0);
    assert(response.has_error &&
           response.error.code == H2_GIZCLAW_RPC_ERROR_UNIMPLEMENTED);
  }
  gizclaw_rpc_v1_SpeakerHwdWriteRequest speaker_patch = {
      .has_volume_percent = true, .volume_percent = 13u};
  uint8_t inner_bytes[64];
  pb_ostream_t inner_out = pb_ostream_from_buffer(inner_bytes,
                                                   sizeof(inner_bytes));
  assert(pb_encode(&inner_out,
                   gizclaw_rpc_v1_SpeakerHwdWriteRequest_fields,
                   &speaker_patch));
  gzc_str_t patch_bytes =
      gzc_str_from_parts((const char *)inner_bytes, inner_out.bytes_written);
  gizclaw_rpc_v1_ClientMhsV0WriteRequest write = {0};
  strcpy(write.id, "speaker.fixture");
  write.hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER;
  write.payload.funcs.encode = encode_bytes;
  write.payload.arg = &patch_bytes;
  uint8_t bytes[512];
  pb_ostream_t out = pb_ostream_from_buffer(bytes, sizeof(bytes));
  assert(pb_encode(&out, gizclaw_rpc_v1_ClientMhsV0WriteRequest_fields,
                   &write));
  response = call(f, H2_GIZCLAW_RPC_CLIENT_MHS_V0_WRITE, bytes,
                  out.bytes_written);
  assert(!response.has_error && f->value == 14);
  gizclaw_rpc_v1_ClientMhsV0ReadRequest read = {0};
  strcpy(read.id, "speaker.fixture");
  read.hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER;
  out = pb_ostream_from_buffer(bytes, sizeof(bytes));
  assert(pb_encode(&out, gizclaw_rpc_v1_ClientMhsV0ReadRequest_fields,
                   &read));
  response = call(f, H2_GIZCLAW_RPC_CLIENT_MHS_V0_READ, bytes,
                  out.bytes_written);
  assert(!response.has_error);
  gzc_str_t read_bytes = {0};
  gizclaw_rpc_v1_ClientMhsV0ReadResponse wrapper_read = {0};
  wrapper_read.payload.funcs.decode = decode_bytes;
  wrapper_read.payload.arg = &read_bytes;
  in = pb_istream_from_buffer((const pb_byte_t *)response.result_payload.data,
                              response.result_payload.len);
  assert(pb_decode(&in, gizclaw_rpc_v1_ClientMhsV0ReadResponse_fields,
                   &wrapper_read));
  gizclaw_rpc_v1_SpeakerHwdReadResponse speaker = {0};
  in = pb_istream_from_buffer((const pb_byte_t *)read_bytes.data,
                              read_bytes.len);
  assert(pb_decode(&in, gizclaw_rpc_v1_SpeakerHwdReadResponse_fields,
                   &speaker));
  assert(speaker.has_volume_percent && speaker.volume_percent == 14u);
  strcpy(read.id, "speaker.unknown");
  out = pb_ostream_from_buffer(bytes, sizeof(bytes));
  assert(pb_encode(&out, gizclaw_rpc_v1_ClientMhsV0ReadRequest_fields,
                   &read));
  response = call(f, H2_GIZCLAW_RPC_CLIENT_MHS_V0_READ, bytes,
                  out.bytes_written);
  assert(response.has_error &&
         response.error.code == H2_GIZCLAW_RPC_ERROR_NOT_FOUND);
  f->exercised = true;
}
static h2_pal_result_t
create_channel(h2_pal_webrtc_peer_t *peer,
               const h2_pal_webrtc_channel_config_t *config,
               h2_pal_webrtc_channel_t **out) {
  (void)config;
  exercise((wire_fixture_t *)peer);
  *out = NULL;
  return H2_PAL_ERR_IO;
}
int main(void) {
  fixture.value = 7;
  const h2_pal_webrtc_vtable_t webrtc_vtable = {
      .peer_create_with_config = create_peer,
      .peer_create_data_channel = create_channel,
      .peer_set_track = bind_track,
      .peer_unset_track = bind_track,
      .peer_poll = poll_peer,
      .channel_send = send_bytes,
      .channel_close = close_channel,
      .peer_close = close_peer};
  const h2_pal_webrtc_api_t webrtc = {.user = &fixture,
                                      .vtable = &webrtc_vtable};
  const h2_pal_http_api_t http = {0};
  const h2_pal_crypto_api_t crypto = {0};
  const h2_gizclaw_tool_handler_t tool = {H2_GIZCLAW_TOOL_DEVICE_FIND,
                                          find_tool, &fixture};
  const h2_gizclaw_mhs_device_t device = {
      .id = "speaker.fixture",
      .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER,
      .read = read_state, .write = write_state, .user = &fixture};
  const h2_gizclaw_config_t config = {.allocator = &memory,
                                      .http = &http,
                                      .crypto = &crypto,
                                      .webrtc = &webrtc,
                                      .time = h2_desktop_platform_time_api(),
                                      .server_endpoint = {"127.0.0.1:1", 11},
                                      .private_key = {"test", 4},
                                      .cipher_mode =
                                          H2_GIZCLAW_CIPHER_PLAINTEXT,
                                      .connect_timeout_ms = 1000,
                                      .model = "fixture",
                                      .tool_handlers = &tool,
                                      .tool_handler_count = 1,
                                      .mhs_devices = &device,
                                      .mhs_device_count = 1};
  const h2_gizclaw_service_config_t service_config = {
      .client_config = &config,
      .task = h2_desktop_platform_task_api(),
      .queue = h2_desktop_platform_queue_api(),
      .sync = h2_desktop_platform_sync_api(),
      .operation_capacity = 4,
      .client_poll_timeout_ms = 1};
  h2_gizclaw_service_t *service = NULL;
  assert(h2_gizclaw_service_init(&service_config, &service) == H2_PAL_OK);
  assert(h2_gizclaw_client_init(&service->client_config, &fixture.client) ==
         H2_PAL_OK);
  assert(gzc_client_connect(h2_gizclaw_client_gzc_internal(fixture.client)) !=
         GZC_OK);
  assert(fixture.exercised);
  h2_gizclaw_client_deinit(fixture.client);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  return 0;
}
