#include "h2_gizclaw_mhs_internal.h"
#include "pb_decode.h"
#include "pb_encode.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  unsigned reads, writes;
  uint32_t brightness;
  bool enabled;
  int failure;
} fixture_t;
static fixture_t fixture = {.brightness = 50u, .enabled = true};
static unsigned live_allocations;
static void *allocate(void *user, size_t size) {
  (void)user;
  void *result = malloc(size);
  if (result != NULL)
    ++live_allocations;
  return result;
}
static void release(void *user, void *pointer) {
  (void)user;
  if (pointer != NULL) {
    assert(live_allocations != 0u);
    --live_allocations;
    free(pointer);
  }
}
static const h2_pal_mem_vtable_t memory_vtable = {
    .alloc = allocate, .free = release};
static const h2_pal_mem_api_t memory = {.vtable = &memory_vtable};

static uint32_t pal_volume = 60u;
static int pal_get_volume(void *user, uint32_t *out) {
  (void)user;
  *out = pal_volume;
  return H2_PAL_OK;
}
static int pal_set_volume(void *user, uint32_t percent) {
  (void)user;
  pal_volume = percent;
  return H2_PAL_OK;
}

/* Without a Runtime the speaker mutes by writing 0: an unmute naming no
 * audible level is refused instead of reported as done while still silent. */
static void test_pal_only_speaker_mute(void) {
  static const h2_pal_audio_vtable_t audio_vtable = {
      .get_speaker_volume_percent = pal_get_volume,
      .set_speaker_volume_percent = pal_set_volume};
  const h2_pal_audio_api_t audio = {.vtable = &audio_vtable};
  h2_gizclaw_mhs_builtin_t builtin = {.audio = &audio};
  h2_gizclaw_mhs_device_t builtins[2];
  assert(h2_gizclaw_mhs_builtins_internal(&builtin, builtins) == 1u);
  const h2_gizclaw_mhs_device_t *speaker = &builtins[0];
  assert(strcmp(speaker->id, "speaker.main") == 0 && speaker->write != NULL);
  h2_gizclaw_mhs_write_t request;
  h2_gizclaw_mhs_read_t out;

  memset(&request, 0, sizeof(request));
  request.speaker.has_muted = true;
  request.speaker.muted = true;
  assert(speaker->write(speaker->user, &request, &out) == H2_PAL_OK);
  assert(pal_volume == 0u && out.speaker.muted);

  request.speaker.muted = false;
  memset(&out, 0, sizeof(out));
  assert(speaker->write(speaker->user, &request, &out) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(pal_volume == 0u);

  request.speaker.has_volume_percent = true;
  request.speaker.volume_percent = 40u;
  assert(speaker->write(speaker->user, &request, &out) == H2_PAL_OK);
  assert(pal_volume == 40u && !out.speaker.muted &&
         out.speaker.volume_percent == 40u);

  /* A plain level change, including to 0, stays allowed. */
  memset(&request, 0, sizeof(request));
  request.speaker.has_volume_percent = true;
  request.speaker.volume_percent = 0u;
  assert(speaker->write(speaker->user, &request, &out) == H2_PAL_OK);
  assert(pal_volume == 0u);
}

static int read_display(void *user, h2_gizclaw_mhs_read_t *out) {
  fixture_t *f = user;
  ++f->reads;
  if (f->failure != H2_PAL_OK)
    return f->failure;
  out->display.has_brightness_percent = true;
  out->display.brightness_percent = f->brightness;
  out->display.has_enabled = true;
  out->display.enabled = f->enabled;
  return H2_PAL_OK;
}
static int write_display(void *user, const h2_gizclaw_mhs_write_t *request,
                         h2_gizclaw_mhs_read_t *out) {
  fixture_t *f = user;
  ++f->writes;
  if (request->display.has_brightness_percent)
    f->brightness = request->display.brightness_percent;
  if (request->display.has_enabled)
    f->enabled = request->display.enabled;
  return read_display(user, out);
}
static h2_gizclaw_mhs_device_t device = {
    .id = "display.main",
    .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY,
    .read = read_display,
    .write = write_display,
    .user = &fixture};

static size_t request_bytes(uint8_t *bytes, bool write, const char *id,
                            gizclaw_rpc_v1_ClientHwd hwd,
                            const void *payload) {
  pb_ostream_t out = pb_ostream_from_buffer(bytes, 256u);
  assert(pb_encode_tag(&out, PB_WT_STRING, 1u));
  assert(pb_encode_string(&out, (const pb_byte_t *)id, strlen(id)));
  assert(pb_encode_tag(&out, PB_WT_VARINT, 2u));
  assert(pb_encode_varint(&out, (uint64_t)hwd));
  if (write) {
    uint8_t inner[64];
    pb_ostream_t payload_out = pb_ostream_from_buffer(inner, sizeof(inner));
    assert(pb_encode(&payload_out,
                     gizclaw_rpc_v1_DisplayHwdWriteRequest_fields, payload));
    assert(pb_encode_tag(&out, PB_WT_STRING, 3u));
    assert(pb_encode_string(&out, inner, payload_out.bytes_written));
  }
  return out.bytes_written;
}
static int invoke(bool write, const uint8_t *bytes, size_t len,
                  const pb_msgdesc_t *reply_fields, void *out) {
  h2_gizclaw_rpc_provider_response_t response = {0};
  uint8_t *storage = NULL;
  int rc = h2_gizclaw_mhs_request_internal(
      &device, 1u, write, &memory,
      (h2_gizclaw_rpc_bytes_t){bytes, len}, &response, &storage);
  if (rc == H2_PAL_OK) {
    pb_istream_t input =
        pb_istream_from_buffer(response.payload.data, response.payload.len);
    pb_wire_type_t wire;
    uint32_t tag = 0u;
    bool eof = false;
    assert(pb_decode_tag(&input, &wire, &tag, &eof));
    assert(wire == PB_WT_STRING && tag == 1u);
    pb_istream_t nested;
    assert(pb_make_string_substream(&input, &nested));
    assert(pb_decode(&nested, reply_fields, out));
    assert(pb_close_string_substream(&input, &nested));
    assert(input.bytes_left == 0u);
  } else {
    assert(storage == NULL && response.payload.data == NULL);
  }
  h2_pal_mem_free(&memory, storage);
  assert(live_allocations == 0u);
  return rc;
}

int main(void) {
  test_pal_only_speaker_mute();
  assert(h2_gizclaw_mhs_validate_internal(&device, 1u) == H2_PAL_OK);
  assert(h2_gizclaw_mhs_validate_internal(NULL, 0u) == H2_PAL_OK);
  assert(h2_gizclaw_mhs_validate_internal(NULL, 1u) == H2_PAL_ERR_INVALID_ARG);
  h2_gizclaw_mhs_device_t duplicate[] = {device, device};
  assert(h2_gizclaw_mhs_validate_internal(duplicate, 2u) ==
         H2_PAL_ERR_INVALID_ARG);
  duplicate[1].id = "display.bad_underscore";
  assert(h2_gizclaw_mhs_validate_internal(duplicate, 2u) ==
         H2_PAL_ERR_INVALID_ARG);
  duplicate[1].id = "battery.main";
  duplicate[1].hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_BATTERY;
  assert(h2_gizclaw_mhs_validate_internal(duplicate, 2u) ==
         H2_PAL_ERR_INVALID_ARG);

  uint8_t bytes[256];
  size_t len = request_bytes(bytes, false, "display.main",
                             gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY,
                             NULL);
  gizclaw_rpc_v1_DisplayHwdReadResponse read = {0};
  assert(invoke(false, bytes, len,
                gizclaw_rpc_v1_DisplayHwdReadResponse_fields,
                &read) == H2_PAL_OK);
  assert(read.has_brightness_percent && read.brightness_percent == 50u &&
         read.has_enabled && read.enabled && fixture.reads == 1u);

  gizclaw_rpc_v1_DisplayHwdWriteRequest patch = {
      .has_brightness_percent = true, .brightness_percent = 42u,
      .has_enabled = true, .enabled = false};
  len = request_bytes(bytes, true, "display.main",
                      gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY, &patch);
  gizclaw_rpc_v1_DisplayHwdWriteResponse write = {0};
  assert(invoke(true, bytes, len,
                gizclaw_rpc_v1_DisplayHwdWriteResponse_fields,
                &write) == H2_PAL_OK);
  assert(write.has_applied && write.applied.has_brightness_percent &&
         write.applied.brightness_percent == 42u &&
         write.applied.has_enabled && !write.applied.enabled &&
         fixture.writes == 1u);

  patch = (gizclaw_rpc_v1_DisplayHwdWriteRequest){0};
  len = request_bytes(bytes, true, "display.main",
                      gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY, &patch);
  assert(invoke(true, bytes, len, NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(fixture.writes == 1u);
  patch.has_brightness_percent = true;
  patch.brightness_percent = 101u;
  len = request_bytes(bytes, true, "display.main",
                      gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY, &patch);
  assert(invoke(true, bytes, len, NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(fixture.writes == 1u);

  len = request_bytes(bytes, false, "missing.main",
                      gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY, NULL);
  assert(invoke(false, bytes, len, NULL, NULL) == H2_PAL_ERR_NOT_FOUND);
  len = request_bytes(bytes, false, "display.main",
                      gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_LED, NULL);
  assert(invoke(false, bytes, len, NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(invoke(false, bytes, len - 1u, NULL, NULL) == H2_PAL_ERR_INVALID_ARG);

  fixture.failure = H2_PAL_ERR_UNAVAILABLE;
  len = request_bytes(bytes, false, "display.main",
                      gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY, NULL);
  assert(invoke(false, bytes, len, NULL, NULL) == H2_PAL_ERR_UNAVAILABLE);

  /* Wire codes: not-yet-published or closing state is retryable. */
  assert(h2_gizclaw_mhs_error_internal(H2_PAL_ERR_UNAVAILABLE) ==
         H2_GIZCLAW_RPC_ERROR_UNAVAILABLE);
  assert(h2_gizclaw_mhs_error_internal(H2_PAL_ERR_CLOSED) ==
         H2_GIZCLAW_RPC_ERROR_UNAVAILABLE);
  assert(h2_gizclaw_mhs_error_internal(H2_PAL_ERR_NOT_FOUND) ==
         H2_GIZCLAW_RPC_ERROR_NOT_FOUND);
  assert(h2_gizclaw_mhs_error_internal(H2_PAL_ERR_INVALID_ARG) ==
         H2_GIZCLAW_RPC_ERROR_INVALID_ARGUMENT);
  assert(h2_gizclaw_mhs_error_internal(H2_PAL_ERR_INVALID_STATE) ==
         H2_GIZCLAW_RPC_ERROR_FAILED_PRECONDITION);
  assert(h2_gizclaw_mhs_error_internal(H2_PAL_ERR_BUSY) ==
         H2_GIZCLAW_RPC_ERROR_RESOURCE_EXHAUSTED);
  assert(h2_gizclaw_mhs_error_internal(H2_PAL_ERR_IO) ==
         H2_GIZCLAW_RPC_ERROR_INTERNAL);
  return 0;
}
