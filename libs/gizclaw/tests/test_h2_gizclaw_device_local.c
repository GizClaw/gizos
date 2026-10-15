#include "h2_desktop_platform.h"
#include "h2_gizclaw_device.h"
#include "h2_gizclaw_service.h"
#include "payload/tool.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic int live_allocations;
static void *allocate(void *u, size_t n) {
  (void)u;
  void *p = malloc(n);
  if (p) {
    memset(p, 0xa5, n);
    ++live_allocations;
  }
  return p;
}
static void *resize(void *u, void *p, size_t n) {
  (void)u;
  void *v = realloc(p, n);
  if (!p && v)
    ++live_allocations;
  return v;
}
static void release(void *u, void *p) {
  (void)u;
  if (p) {
    --live_allocations;
    free(p);
  }
}
static const h2_pal_mem_vtable_t memory_vtable = {allocate, resize, release};
static const h2_pal_mem_api_t memory = {.vtable = &memory_vtable};
static const h2_pal_task_api_t *base_task;
static h2_pal_task_t *rpc_task;
static int fail_join, fail_cond_destroy, fail_after_join, fail_service_mutex_destroy;
static _Atomic int fail_rpc_lock;
static h2_pal_mutex_t *rpc_mutex, *service_mutex;
static unsigned device_starts;
static const h2_pal_sync_api_t *base_sync;
static int create_mutex(void *user, const h2_pal_mutex_config_t *config,
                        h2_pal_mutex_t **out) {
  (void)user;
  int rc = h2_pal_mutex_create(base_sync, config, out);
  if (!rc && !strcmp(config->name, "gizclaw-device-rpc")) rpc_mutex = *out;
  if (!rc && !strcmp(config->name, "gizclaw-service") && !service_mutex)
    service_mutex = *out;
  return rc;
}
static int destroy_mutex(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  if (mutex == service_mutex && fail_service_mutex_destroy) {
    fail_service_mutex_destroy = 0;
    return H2_PAL_ERR_IO;
  }
  int rc = h2_pal_mutex_destroy(base_sync, mutex);
  if (!rc && mutex == service_mutex) service_mutex = NULL;
  return rc;
}
static int lock_mutex(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  if (mutex == rpc_mutex && atomic_exchange(&fail_rpc_lock, 0))
    return H2_PAL_ERR_IO;
  return h2_pal_mutex_lock(base_sync, mutex);
}
static int destroy_cond(void *user, h2_pal_cond_t *cond) {
  (void)user;
  if (fail_cond_destroy) {
    fail_cond_destroy = 0;
    return H2_PAL_ERR_IO;
  }
  return h2_pal_cond_destroy(base_sync, cond);
}
static int task_start(void *u, const h2_pal_task_options_t *o,
                      h2_pal_task_entry_t e, void *c, h2_pal_task_t **out) {
  (void)u;
  int rc = h2_pal_task_start(base_task, o, e, c, out);
  if (!rc && !strcmp(o->name, "$gizclaw/device"))
    ++device_starts;
  if (!rc && !strcmp(o->name, "$gizclaw/device/rpc"))
    rpc_task = *out;
  return rc;
}
static int task_join(void *u, h2_pal_task_t *t) {
  (void)u;
  if (t == rpc_task && fail_join) {
    fail_join = 0;
    return H2_PAL_ERR_IO;
  }
  int rc = h2_pal_task_join(base_task, t);
  if (!rc && t == rpc_task && fail_after_join) {
    fail_after_join = 0;
    atomic_store(&fail_rpc_lock, 1);
  }
  return rc;
}
static const h2_pal_task_vtable_t task_vtable = {task_start, task_join};
static const h2_pal_task_api_t tasks = {.vtable = &task_vtable};
typedef struct fixture {
  uint32_t volume;
  bool muted;
  _Atomic int reads, writes, completions, completion_result, entered, gate,
      error;
  pthread_t invoke_thread, completion_thread;
} fixture_t;
static int speaker_read(void *u, h2_gizclaw_mhs_read_t *out) {
  fixture_t *f = u;
  ++f->reads;
  out->speaker =
      (gizclaw_rpc_v1_SpeakerHwdReadResponse){.has_volume_percent = true,
                                              .volume_percent = f->volume,
                                              .has_muted = true,
                                              .muted = f->muted};
  return H2_PAL_OK;
}
static int speaker_write(void *u, const h2_gizclaw_mhs_write_t *in,
                         h2_gizclaw_mhs_read_t *out) {
  fixture_t *f = u;
  ++f->writes;
  if (in->speaker.has_volume_percent)
    f->volume = in->speaker.volume_percent;
  if (in->speaker.has_muted)
    f->muted = in->speaker.muted;
  return speaker_read(u, out);
}
static int wifi_read(void *u, h2_gizclaw_mhs_read_t *out) {
  (void)u;
  out->wifi = (gizclaw_rpc_v1_WifiHwdReadResponse){.has_connected = true,
                                                   .connected = false};
  return H2_PAL_OK;
}
static void completed(void *u, int rc) {
  fixture_t *f = u;
  f->completion_thread = pthread_self();
  atomic_store(&f->completion_result, rc);
  ++f->completions;
}
static int product_find(void *u, h2_gizclaw_tool_t tool,
                        h2_gizclaw_rpc_bytes_t in,
                        h2_gizclaw_rpc_provider_response_t *out) {
  (void)in;
  assert(tool == H2_GIZCLAW_TOOL_DEVICE_FIND);
  fixture_t *f = u;
  f->invoke_thread = pthread_self();
  atomic_store(&f->entered, 1);
  while (atomic_load(&f->gate))
    assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u) ==
           H2_PAL_OK);
  if (atomic_load(&f->error))
    return -1234;
  out->on_complete = completed;
  out->complete_user = f;
  return H2_PAL_OK;
}
typedef struct bytes {
  const uint8_t *data;
  size_t size;
} bytes_t;
static bool put_bytes(pb_ostream_t *s, const pb_field_t *f, void *const *u) {
  const bytes_t *b = *u;
  return pb_encode_tag_for_field(s, f) && pb_encode_string(s, b->data, b->size);
}
static bool get_bytes(pb_istream_t *s, const pb_field_t *f, void **u) {
  (void)f;
  bytes_t *b = *u;
  b->data = s->state;
  b->size = s->bytes_left;
  return pb_read(s, NULL, s->bytes_left);
}
static size_t encode(uint8_t *out, size_t capacity, const pb_msgdesc_t *desc,
                     const void *value) {
  pb_ostream_t stream = pb_ostream_from_buffer(out, capacity);
  assert(pb_encode(&stream, desc, value));
  return stream.bytes_written;
}
static h2_gizclaw_device_call_t *submit(h2_gizclaw_device_t *d, int method,
                                        const uint8_t *p, size_t n) {
  h2_gizclaw_device_call_t *c = NULL;
  assert(h2_gizclaw_device_submit(d, method, (h2_gizclaw_rpc_bytes_t){p, n},
                                  5000u, &c) == H2_PAL_OK);
  return c;
}
static h2_gizclaw_device_result_t wait_call(h2_gizclaw_device_call_t *c) {
  h2_gizclaw_device_result_t view;
  assert(h2_gizclaw_device_call_wait(c, 5000u, &view) == H2_PAL_OK);
  return view;
}
static void done(h2_gizclaw_device_call_t **c) {
  assert(h2_gizclaw_device_call_complete(*c, H2_PAL_OK) == H2_PAL_OK);
  assert(h2_gizclaw_device_call_release(c) == H2_PAL_OK && *c == NULL);
}
static void wait_counter(_Atomic int *value, int target) {
  for (unsigned i = 0; i < 5000 && atomic_load(value) < target; ++i)
    assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u) ==
           H2_PAL_OK);
  assert(atomic_load(value) == target);
}
int main(void) {
  base_task = h2_desktop_platform_task_api();
  base_sync = h2_desktop_platform_sync_api();
  h2_pal_sync_vtable_t sync_vtable = *base_sync->vtable;
  sync_vtable.destroy_cond = destroy_cond;
  sync_vtable.create_mutex = create_mutex;
  sync_vtable.destroy_mutex = destroy_mutex;
  sync_vtable.lock_mutex = lock_mutex;
  const h2_pal_sync_api_t sync = {.user = base_sync->user,
                                  .vtable = &sync_vtable};
  fixture_t f = {.volume = 77u, .muted = true};
  const h2_gizclaw_tool_handler_t tool = {H2_GIZCLAW_TOOL_DEVICE_FIND,
                                          product_find, &f};
  const h2_gizclaw_mhs_device_t devices[] = {
      {.id = "speaker.fixture",
       .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER,
       .read = speaker_read,
       .write = speaker_write,
       .user = &f},
      {.id = "wifi.fixture",
       .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_WIFI,
       .read = wifi_read}};
  const h2_gizclaw_config_t config = {.allocator = &memory,
                                      .time = h2_desktop_platform_time_api(),
                                      .model = "local-fixture",
                                      .tool_handlers = &tool,
                                      .tool_handler_count = 1,
                                      .mhs_devices = devices,
                                      .mhs_device_count = 2};
  const h2_gizclaw_device_config_t setup = {.client_config = &config,
                                            .task = &tasks,
                                            .sync = &sync,
                                            .request_capacity = 4};
  h2_gizclaw_device_t *d = NULL;
  assert(h2_gizclaw_device_create(&setup, &d) == H2_PAL_OK && d);
  assert(h2_gizclaw_device_start(d) == H2_PAL_OK);
  assert(h2_gizclaw_device_retain(d) == H2_PAL_OK);
  h2_gizclaw_device_t *borrow = d;
  assert(h2_gizclaw_device_stop(d) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_device_release(&borrow) == H2_PAL_OK && !borrow);
  h2_gizclaw_device_call_t *c =
      submit(d, H2_GIZCLAW_RPC_CLIENT_TOOL_V0_LIST, NULL, 0);
  h2_gizclaw_device_result_t view = wait_call(c);
  assert(view.result == H2_PAL_OK && !view.has_error);
  gizclaw_rpc_v1_ClientToolV0ListResponse list = {0};
  pb_istream_t input =
      pb_istream_from_buffer(view.payload.data, view.payload.len);
  assert(pb_decode(&input, gizclaw_rpc_v1_ClientToolV0ListResponse_fields,
                   &list) &&
         list.tools_count == 4);
  bool found = false;
  for (size_t i = 0; i < list.tools_count; ++i)
    if (list.tools[i] == gizclaw_rpc_v1_ClientTool_CLIENT_TOOL_DEVICE_FIND)
      found = true;
  assert(found);
  done(&c);
  uint8_t inner[64], request[512];
  gizclaw_rpc_v1_SpeakerHwdWriteRequest write = {.has_volume_percent = true,
                                                 .volume_percent = 0,
                                                 .has_muted = true,
                                                 .muted = false};
  bytes_t payload = {inner, encode(inner, sizeof(inner),
                                   gizclaw_rpc_v1_SpeakerHwdWriteRequest_fields,
                                   &write)};
  gizclaw_rpc_v1_ClientMhsV0WriteRequest mhs = {
      .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER,
      .payload = {.funcs.encode = put_bytes, .arg = &payload}};
  strcpy(mhs.id, "speaker.fixture");
  size_t length = encode(request, sizeof(request),
                         gizclaw_rpc_v1_ClientMhsV0WriteRequest_fields, &mhs);
  c = submit(d, H2_GIZCLAW_RPC_CLIENT_MHS_V0_WRITE, request, length);
  memset(request, 0, sizeof(request));
  view = wait_call(c);
  assert(view.result == H2_PAL_OK && !view.has_error && f.volume == 0 &&
         !f.muted);
  bytes_t applied = {0};
  gizclaw_rpc_v1_ClientMhsV0WriteResponse result = {
      .payload = {.funcs.decode = get_bytes, .arg = &applied}};
  input = pb_istream_from_buffer(view.payload.data, view.payload.len);
  assert(pb_decode(&input, gizclaw_rpc_v1_ClientMhsV0WriteResponse_fields,
                   &result));
  gizclaw_rpc_v1_SpeakerHwdWriteResponse speaker = {0};
  input = pb_istream_from_buffer(applied.data, applied.size);
  assert(pb_decode(&input, gizclaw_rpc_v1_SpeakerHwdWriteResponse_fields,
                   &speaker));
  assert(speaker.has_applied && speaker.applied.has_volume_percent &&
         speaker.applied.volume_percent == 0 && speaker.applied.has_muted &&
         !speaker.applied.muted);
  /* A later response must not overwrite a retained earlier result view. */
  uint8_t earlier[512];
  assert(view.payload.len <= sizeof(earlier));
  memcpy(earlier, view.payload.data, view.payload.len);
  write.volume_percent = 33;
  payload.size = encode(inner, sizeof(inner),
                        gizclaw_rpc_v1_SpeakerHwdWriteRequest_fields, &write);
  length = encode(request, sizeof(request),
                  gizclaw_rpc_v1_ClientMhsV0WriteRequest_fields, &mhs);
  h2_gizclaw_device_call_t *second =
      submit(d, H2_GIZCLAW_RPC_CLIENT_MHS_V0_WRITE, request, length);
  h2_gizclaw_device_result_t second_view = wait_call(second);
  assert(second_view.result == H2_PAL_OK && f.volume == 33 &&
         second_view.payload.data != view.payload.data &&
         memcmp(earlier, view.payload.data, view.payload.len) == 0);
  done(&second);
  done(&c);
  write.volume_percent = 101;
  payload.size = encode(inner, sizeof(inner),
                        gizclaw_rpc_v1_SpeakerHwdWriteRequest_fields, &write);
  length = encode(request, sizeof(request),
                  gizclaw_rpc_v1_ClientMhsV0WriteRequest_fields, &mhs);
  c = submit(d, H2_GIZCLAW_RPC_CLIENT_MHS_V0_WRITE, request, length);
  view = wait_call(c);
  assert(view.result == H2_PAL_ERR_INVALID_ARG && view.has_error &&
         view.error_code == 3 && atomic_load(&f.writes) == 2);
  done(&c);
  gizclaw_rpc_v1_ClientMhsV0ReadRequest read = {
      .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER};
  strcpy(read.id, "absent.fixture");
  length = encode(request, sizeof(request),
                  gizclaw_rpc_v1_ClientMhsV0ReadRequest_fields, &read);
  c = submit(d, H2_GIZCLAW_RPC_CLIENT_MHS_V0_READ, request, length);
  view = wait_call(c);
  assert(view.result == H2_PAL_ERR_NOT_FOUND && view.has_error &&
         view.error_code == 5);
  done(&c);
  mhs.hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_WIFI;
  strcpy(mhs.id, "wifi.fixture");
  length = encode(request, sizeof(request),
                  gizclaw_rpc_v1_ClientMhsV0WriteRequest_fields, &mhs);
  c = submit(d, H2_GIZCLAW_RPC_CLIENT_MHS_V0_WRITE, request, length);
  view = wait_call(c);
  assert(view.result == H2_PAL_ERR_UNSUPPORTED && view.has_error &&
         view.error_code == 12);
  done(&c);
  c = submit(d, 9999, NULL, 0);
  view = wait_call(c);
  assert(view.has_error && view.error_code == 12);
  done(&c);
  const uint8_t malformed[] = {0x0a, 0xff};
  c = submit(d, H2_GIZCLAW_RPC_CLIENT_MHS_V0_READ, malformed,
             sizeof(malformed));
  view = wait_call(c);
  assert(view.has_error && view.error_code == 3);
  done(&c);
  gizclaw_rpc_v1_ClientToolV0InvokeRequest invoke = {
      .tool = gizclaw_rpc_v1_ClientTool_CLIENT_TOOL_DEVICE_FIND};
  length = encode(request, sizeof(request),
                  gizclaw_rpc_v1_ClientToolV0InvokeRequest_fields, &invoke);
  atomic_store(&f.gate, 1);
  c = submit(d, H2_GIZCLAW_RPC_CLIENT_TOOL_V0_INVOKE, request, length);
  wait_counter(&f.entered, 1);
  h2_gizclaw_device_call_t *queued =
      submit(d, H2_GIZCLAW_RPC_CLIENT_MHS_V0_READ, request, 0);
  assert(h2_gizclaw_device_call_cancel(queued) == H2_PAL_OK);
  assert(h2_gizclaw_device_call_release(&queued) == H2_PAL_OK);
  assert(h2_gizclaw_device_call_wait(c, 1, &view) == H2_PAL_ERR_TIMEOUT);
  atomic_store(&f.gate, 0);
  view = wait_call(c);
  assert(view.result == H2_PAL_OK);
  assert(atomic_load(&f.completions) == 0);
  assert(h2_gizclaw_device_call_complete(c, H2_PAL_OK) == H2_PAL_OK);
  assert(h2_gizclaw_device_call_complete(c, H2_PAL_OK) == H2_PAL_ERR_CLOSED);
  wait_counter(&f.completions, 1);
  assert(h2_gizclaw_device_call_cancel(c) == H2_PAL_ERR_CLOSED);
  assert(h2_gizclaw_device_call_release(&c) == H2_PAL_OK);
  assert(atomic_load(&f.completion_result) == H2_PAL_OK &&
         pthread_equal(f.invoke_thread, f.completion_thread));
  atomic_store(&f.error, 1);
  c = submit(d, H2_GIZCLAW_RPC_CLIENT_TOOL_V0_INVOKE, request, length);
  view = wait_call(c);
  assert(view.result == -1234 && view.has_error);
  done(&c);
  atomic_store(&f.error, 0);
  h2_gizclaw_service_config_t service_setup = {
      .client_config = &config,
      .task = &tasks,
      .queue = h2_desktop_platform_queue_api(),
      .sync = setup.sync,
      .operation_capacity = 4,
      .client_poll_timeout_ms = 1,
      .device = d};
  for (int i = 0; i < 3; ++i) {
    h2_gizclaw_service_t *service = NULL;
    assert(h2_gizclaw_service_init(&service_setup, &service) == H2_PAL_OK);
    assert(h2_gizclaw_device_stop(d) == H2_PAL_ERR_BUSY);
    const unsigned starts_before = device_starts;
    if (i == 1)
      assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    assert(device_starts == starts_before);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    size_t dispatched = 0;
    assert(h2_gizclaw_service_poll(service, 64u, &dispatched) == H2_PAL_OK);
    if (i == 0) {
      fail_cond_destroy = 1;
      assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_IO);
    }
    if (i == 2) {
      fail_service_mutex_destroy = 1;
      assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_IO);
    }
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
    c = submit(d, H2_GIZCLAW_RPC_CLIENT_TOOL_V0_LIST, NULL, 0);
    view = wait_call(c);
    assert(view.result == H2_PAL_OK && !view.has_error);
    done(&c);
  }
  h2_gizclaw_device_call_t *held[4];
  for (size_t i = 0; i < 4; ++i) {
    held[i] = submit(d, H2_GIZCLAW_RPC_CLIENT_TOOL_V0_LIST, NULL, 0);
    (void)wait_call(held[i]);
  }
  assert(h2_gizclaw_device_submit(d, 136, (h2_gizclaw_rpc_bytes_t){0}, 1000,
                                  &c) == H2_PAL_ERR_BUSY &&
         !c);
  assert(h2_gizclaw_device_stop(d) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_device_destroy(&d) == H2_PAL_ERR_BUSY && d);
  for (size_t i = 0; i < 4; ++i)
    assert(h2_gizclaw_device_call_release(&held[i]) == H2_PAL_OK);
  assert(h2_gizclaw_device_stop(d) == H2_PAL_OK);
  assert(h2_gizclaw_device_start(d) == H2_PAL_OK);
  fail_join = 1;
  assert(h2_gizclaw_device_stop(d) == H2_PAL_ERR_IO);
  assert(h2_gizclaw_device_destroy(&d) == H2_PAL_ERR_BUSY && d);
  assert(h2_gizclaw_device_stop(d) == H2_PAL_OK);
  assert(h2_gizclaw_device_start(d) == H2_PAL_OK);
  fail_after_join = 1;
  assert(h2_gizclaw_device_stop(d) == H2_PAL_ERR_IO);
  assert(h2_gizclaw_device_destroy(&d) == H2_PAL_ERR_BUSY && d);
  /* The successful join consumed its handle despite the later mutex error. */
  assert(h2_gizclaw_device_stop(d) == H2_PAL_OK);
  assert(h2_gizclaw_device_destroy(&d) == H2_PAL_OK && !d);
  assert(atomic_load(&live_allocations) == 0);
  return 0;
}
