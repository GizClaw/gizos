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
static void* allocate(void* user, size_t size) {
  (void)user;
  void* result = malloc(size);
  if (result != NULL) ++live_allocations;
  return result;
}
static void release(void* user, void* pointer) {
  (void)user;
  if (pointer != NULL) {
    assert(live_allocations != 0u);
    --live_allocations;
    free(pointer);
  }
}
static const h2_pal_mem_vtable_t memory_vtable = {.alloc = allocate,
                                                  .free = release};
static const h2_pal_mem_api_t memory = {.vtable = &memory_vtable};

static uint32_t pal_volume = 60u;
static int pal_get_volume(void* user, uint32_t* out) {
  (void)user;
  *out = pal_volume;
  return H2_PAL_OK;
}
static int pal_set_volume(void* user, uint32_t percent) {
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
  const h2_gizclaw_mhs_device_t* speaker = &builtins[0];
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

static int read_display(void* user, h2_gizclaw_mhs_read_t* out) {
  fixture_t* f = user;
  ++f->reads;
  if (f->failure != H2_PAL_OK) return f->failure;
  out->display.has_brightness_percent = true;
  out->display.brightness_percent = f->brightness;
  out->display.has_enabled = true;
  out->display.enabled = f->enabled;
  return H2_PAL_OK;
}
static int write_display(void* user, const h2_gizclaw_mhs_write_t* request,
                         h2_gizclaw_mhs_read_t* out) {
  fixture_t* f = user;
  ++f->writes;
  if (request->display.has_brightness_percent)
    f->brightness = request->display.brightness_percent;
  if (request->display.has_enabled) f->enabled = request->display.enabled;
  return read_display(user, out);
}
static h2_gizclaw_mhs_device_t device = {
    .id = "display.main",
    .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY,
    .read = read_display,
    .write = write_display,
    .user = &fixture};

static size_t request_bytes(uint8_t* bytes, bool write, const char* id,
                            gizclaw_rpc_v1_ClientHwd hwd, const void* payload) {
  pb_ostream_t out = pb_ostream_from_buffer(bytes, 256u);
  assert(pb_encode_tag(&out, PB_WT_STRING, 1u));
  assert(pb_encode_string(&out, (const pb_byte_t*)id, strlen(id)));
  assert(pb_encode_tag(&out, PB_WT_VARINT, 2u));
  assert(pb_encode_varint(&out, (uint64_t)hwd));
  if (write) {
    uint8_t inner[64];
    pb_ostream_t payload_out = pb_ostream_from_buffer(inner, sizeof(inner));
    const pb_msgdesc_t* fields =
        hwd == gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_MODEM
            ? gizclaw_rpc_v1_ModemHwdWriteRequest_fields
        : hwd == gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_NFC
            ? gizclaw_rpc_v1_NfcHwdWriteRequest_fields
            : gizclaw_rpc_v1_DisplayHwdWriteRequest_fields;
    assert(pb_encode(&payload_out, fields, payload));
    assert(pb_encode_tag(&out, PB_WT_STRING, 3u));
    assert(pb_encode_string(&out, inner, payload_out.bytes_written));
  }
  return out.bytes_written;
}
static int invoke_devices(const h2_gizclaw_mhs_device_t* devices, size_t count,
                          bool write, const uint8_t* bytes, size_t len,
                          const pb_msgdesc_t* reply_fields, void* out) {
  h2_gizclaw_rpc_provider_response_t response = {0};
  uint8_t* storage = NULL;
  int rc = h2_gizclaw_mhs_request_internal(devices, count, write, &memory,
                                           (h2_gizclaw_rpc_bytes_t){bytes, len},
                                           &response, &storage);
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

static int invoke(bool write, const uint8_t* bytes, size_t len,
                  const pb_msgdesc_t* fields, void* out) {
  return invoke_devices(&device, 1u, write, bytes, len, fields, out);
}

typedef struct radio_fixture {
  bool nfc, omit_enabled;
  unsigned writes;
  h2_gizclaw_mhs_read_t snapshot;
} radio_fixture_t;

static int radio_read(void* user, h2_gizclaw_mhs_read_t* out) {
  *out = ((radio_fixture_t*)user)->snapshot;
  return H2_PAL_OK;
}

static int radio_write(void* user, const h2_gizclaw_mhs_write_t* request,
                       h2_gizclaw_mhs_read_t* out) {
  radio_fixture_t* f = user;
  ++f->writes;
  if (f->nfc) {
    assert(request->nfc.has_enabled);
    f->snapshot.nfc.has_enabled = !f->omit_enabled;
    f->snapshot.nfc.enabled = request->nfc.enabled;
  } else {
    assert(request->modem.has_enabled);
    f->snapshot.modem.has_enabled = !f->omit_enabled;
    f->snapshot.modem.enabled = request->modem.enabled;
  }
  return radio_read(user, out);
}

static void test_modem_and_nfc(void) {
  radio_fixture_t modem = {.snapshot.modem = {.has_enabled = true}};
  radio_fixture_t nfc = {.nfc = true,
                         .snapshot.nfc = {.has_enabled = true,
                                          .has_card_present = true,
                                          .has_last_read = true}};
  radio_fixture_t second = {
      .nfc = true, .snapshot.nfc = {.has_enabled = true, .enabled = true}};
  gizclaw_rpc_v1_NfcReadSnapshot* last = &nfc.snapshot.nfc.last_read;
  last->read_at_unix_ms = INT64_C(1800000000000);
  strcpy(last->result, "failed");
  last->has_error_code = true;
  strcpy(last->error_code, "read-failed");
  last->has_uid = true;
  last->uid.size = sizeof(last->uid.bytes);
  last->has_raw_data = true;
  last->raw_data.size = sizeof(last->raw_data.bytes);
  for (size_t i = 0; i < sizeof(last->raw_data.bytes); ++i)
    last->raw_data.bytes[i] = (uint8_t)i;
  h2_gizclaw_mhs_device_t devices[] = {
      {"modem.main", gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_MODEM, radio_read,
       radio_write, &modem},
      {"nfc.main", gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_NFC, radio_read,
       radio_write, &nfc},
      {"nfc.second", gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_NFC, radio_read, NULL,
       &second}};
  assert(h2_gizclaw_mhs_validate_internal(devices, 3u) == H2_PAL_OK);
  uint8_t bytes[256];
  size_t len = request_bytes(bytes, false, "modem.main", devices[0].hwd, NULL);
  gizclaw_rpc_v1_ModemHwdReadResponse read_modem = {0};
  assert(invoke_devices(devices, 3u, false, bytes, len,
                        gizclaw_rpc_v1_ModemHwdReadResponse_fields,
                        &read_modem) == H2_PAL_OK);
  assert(read_modem.has_enabled && !read_modem.enabled &&
         !read_modem.has_powered);
  modem.snapshot.modem.has_powered = true;
  assert(invoke_devices(devices, 3u, false, bytes, len,
                        gizclaw_rpc_v1_ModemHwdReadResponse_fields,
                        &read_modem) == H2_PAL_OK);
  assert(read_modem.has_powered && !read_modem.powered);
  gizclaw_rpc_v1_ModemHwdWriteRequest patch = {.has_enabled = true};
  gizclaw_rpc_v1_ModemHwdWriteResponse applied = {0};
  len = request_bytes(bytes, true, "modem.main", devices[0].hwd, &patch);
  assert(invoke_devices(devices, 3u, true, bytes, len,
                        gizclaw_rpc_v1_ModemHwdWriteResponse_fields,
                        &applied) == H2_PAL_OK);
  assert(applied.has_applied && applied.applied.has_enabled &&
         !applied.applied.enabled && modem.writes == 1u);
  modem.omit_enabled = true;
  assert(invoke_devices(devices, 3u, true, bytes, len, NULL, NULL) ==
         H2_PAL_ERR_IO);
  patch.has_enabled = false;
  len = request_bytes(bytes, true, "modem.main", devices[0].hwd, &patch);
  const unsigned writes = modem.writes;
  assert(invoke_devices(devices, 3u, true, bytes, len, NULL, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(modem.writes == writes);

  /* A full binary snapshot exceeds the previous 512-byte response buffer.
   * Card absence retains the last failed attempt and binary zero bytes. */
  gizclaw_rpc_v1_NfcHwdReadResponse read_nfc = {0};
  len = request_bytes(bytes, false, "nfc.main", devices[1].hwd, NULL);
  assert(invoke_devices(devices, 3u, false, bytes, len,
                        gizclaw_rpc_v1_NfcHwdReadResponse_fields,
                        &read_nfc) == H2_PAL_OK);
  assert(read_nfc.has_card_present && !read_nfc.card_present &&
         read_nfc.has_last_read &&
         !strcmp(read_nfc.last_read.result, "failed"));
  assert(
      read_nfc.last_read.raw_data.size == 1024u &&
      !memcmp(read_nfc.last_read.raw_data.bytes, last->raw_data.bytes, 1024u));
  assert(read_nfc.last_read.uid.size == 32u &&
         read_nfc.last_read.has_error_code);
  last->has_error_code = false;
  assert(invoke_devices(devices, 3u, false, bytes, len, NULL, NULL) ==
         H2_PAL_ERR_IO);
  strcpy(last->result, "success");
  assert(invoke_devices(devices, 3u, false, bytes, len,
                        gizclaw_rpc_v1_NfcHwdReadResponse_fields,
                        &read_nfc) == H2_PAL_OK);

  last->has_error_code = true;
  assert(invoke_devices(devices, 3u, false, bytes, len, NULL, NULL) ==
         H2_PAL_ERR_IO);
  last->has_error_code = false;
  last->has_technology = true;
  strcpy(last->technology, "\xc0\x80");
  assert(invoke_devices(devices, 3u, false, bytes, len, NULL, NULL) ==
         H2_PAL_ERR_IO);
  strcpy(last->technology, "\xe8\xaf\xbb\xe5\x8d\xa1");
  assert(invoke_devices(devices, 3u, false, bytes, len,
                        gizclaw_rpc_v1_NfcHwdReadResponse_fields,
                        &read_nfc) == H2_PAL_OK);
  last->raw_data.size = 1025u;
  assert(invoke_devices(devices, 3u, false, bytes, len, NULL, NULL) ==
         H2_PAL_ERR_IO);
  last->raw_data.size = 1024u;
  gizclaw_rpc_v1_NfcHwdWriteRequest nfc_patch = {.has_enabled = true};
  gizclaw_rpc_v1_NfcHwdWriteResponse nfc_applied = {0};
  len = request_bytes(bytes, true, "nfc.main", devices[1].hwd, &nfc_patch);
  assert(invoke_devices(devices, 3u, true, bytes, len,
                        gizclaw_rpc_v1_NfcHwdWriteResponse_fields,
                        &nfc_applied) == H2_PAL_OK);
  assert(nfc_applied.has_applied && nfc_applied.applied.has_enabled &&
         !nfc_applied.applied.enabled && nfc_applied.applied.has_last_read);
  nfc.omit_enabled = true;
  assert(invoke_devices(devices, 3u, true, bytes, len, NULL, NULL) ==
         H2_PAL_ERR_IO);
  len = request_bytes(bytes, true, "nfc.second", devices[2].hwd, &nfc_patch);
  assert(invoke_devices(devices, 3u, true, bytes, len, NULL, NULL) ==
         H2_PAL_ERR_UNSUPPORTED);
  len = request_bytes(bytes, false, "nfc.second", devices[2].hwd, NULL);
  assert(invoke_devices(devices, 3u, false, bytes, len,
                        gizclaw_rpc_v1_NfcHwdReadResponse_fields,
                        &read_nfc) == H2_PAL_OK);
  assert(read_nfc.enabled && !read_nfc.has_last_read && second.writes == 0u);
  len = request_bytes(bytes, false, "nfc.main", devices[0].hwd, NULL);
  assert(invoke_devices(devices, 3u, false, bytes, len, NULL, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
  test_modem_and_nfc();
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
                             gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY, NULL);
  gizclaw_rpc_v1_DisplayHwdReadResponse read = {0};
  assert(invoke(false, bytes, len, gizclaw_rpc_v1_DisplayHwdReadResponse_fields,
                &read) == H2_PAL_OK);
  assert(read.has_brightness_percent && read.brightness_percent == 50u &&
         read.has_enabled && read.enabled && fixture.reads == 1u);

  gizclaw_rpc_v1_DisplayHwdWriteRequest patch = {.has_brightness_percent = true,
                                                 .brightness_percent = 42u,
                                                 .has_enabled = true,
                                                 .enabled = false};
  len = request_bytes(bytes, true, "display.main",
                      gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY, &patch);
  gizclaw_rpc_v1_DisplayHwdWriteResponse write = {0};
  assert(invoke(true, bytes, len, gizclaw_rpc_v1_DisplayHwdWriteResponse_fields,
                &write) == H2_PAL_OK);
  assert(write.has_applied && write.applied.has_brightness_percent &&
         write.applied.brightness_percent == 42u && write.applied.has_enabled &&
         !write.applied.enabled && fixture.writes == 1u);

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

  /* An ID with an embedded NUL does not address the instance its prefix
   * names: the write is refused and the hardware is not touched. */
  {
    static const char with_nul[] = "display.main\0other";
    uint8_t forged[256];
    pb_ostream_t out = pb_ostream_from_buffer(forged, sizeof(forged));
    assert(pb_encode_tag(&out, PB_WT_STRING, 1u));
    assert(pb_encode_string(&out, (const pb_byte_t*)with_nul,
                            sizeof(with_nul) - 1u));
    assert(pb_encode_tag(&out, PB_WT_VARINT, 2u));
    assert(pb_encode_varint(&out, gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY));
    gizclaw_rpc_v1_DisplayHwdWriteRequest forged_patch = {
        .has_brightness_percent = true, .brightness_percent = 10u};
    uint8_t inner[64];
    pb_ostream_t payload = pb_ostream_from_buffer(inner, sizeof(inner));
    assert(pb_encode(&payload, gizclaw_rpc_v1_DisplayHwdWriteRequest_fields,
                     &forged_patch));
    assert(pb_encode_tag(&out, PB_WT_STRING, 3u));
    assert(pb_encode_string(&out, inner, payload.bytes_written));
    const unsigned writes = fixture.writes;
    assert(invoke(true, forged, out.bytes_written, NULL, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(fixture.writes == writes);
  }

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
