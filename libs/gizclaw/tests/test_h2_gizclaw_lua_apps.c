#include "h2_app_test_fs.h"
#include "h2_atomic.h"
#include "h2_desktop_platform.h"
#include "h2_gizclaw_lua_apps_internal.h"
#include "h2_gizclaw_lua_stream_internal.h"
#include "libs/gizclaw/lua_apps/fixtures.h"
#include "payload/lua_app.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void *allocate(void *user, size_t size) {
  (void)user;
  return malloc(size);
}
static void *resize(void *user, void *ptr, size_t size) {
  (void)user;
  return realloc(ptr, size);
}
static void release(void *user, void *ptr) {
  (void)user;
  free(ptr);
}
static const h2_pal_mem_vtable_t mem_vtable = {allocate, resize, release};
static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
static h2_atomic_bool_t gate_enabled, gate_entered, gate_released;
static unsigned yields;
static int (*base_write)(void *, h2_pal_fs_file_t *, const void *, size_t,
                         size_t *);
static int gated_write(void *user, h2_pal_fs_file_t *file, const void *bytes,
                       size_t size, size_t *written) {
  if (h2_atomic_load(&gate_enabled)) {
    h2_atomic_store(&gate_entered, true);
    while (!h2_atomic_load(&gate_released))
      (void)h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u);
  }
  return base_write(user, file, bytes, size, written);
}
static int yield_to_worker(void *user, uint32_t ms) {
  (void)user;
  assert(ms == 1u);
  ++yields;
  h2_atomic_store(&gate_enabled, false);
  h2_atomic_store(&gate_released, true);
  return h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), ms);
}
static const h2_pal_time_vtable_t progress_time_vtable = {.sleep_ms =
                                                              yield_to_worker};
static const h2_pal_time_api_t progress_time = {.vtable =
                                                    &progress_time_vtable};
static void pause_worker(void) {
  assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u) == 0);
}
static void put(const h2_pal_fs_api_t *fs, const char *path, const void *data,
                size_t size) {
  h2_pal_fs_file_t *file = NULL;
  size_t written = 0;
  assert(h2_pal_fs_open(fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file) == 0);
  assert(h2_pal_fs_write(fs, file, data, size, &written) == 0 &&
         written == size);
  assert(h2_pal_fs_close(fs, file) == 0);
}
static void wait_version(h2_gizclaw_lua_app_catalog_t *apps,
                         const char *version) {
  h2_gizclaw_lua_app_info_t info;
  for (unsigned i = 0; i < 5000; ++i) {
    int rc = h2_gizclaw_lua_app_catalog_get(apps, "demo", &info);
    if (rc == H2_PAL_OK) {
      assert(!strcmp(info.version, version));
      return;
    }
    assert(rc == H2_PAL_ERR_BUSY);
    pause_worker();
  }
  assert(false);
}
static void *open_stream(const gzc_rpc_stream_provider_t *provider, size_t size,
                         const char *sha256) {
  gizclaw_rpc_v1_ClientLuaAppInstallStreamRequest metadata = {.content_length =
                                                                  size};
  strcpy(metadata.sha256, sha256);
  uint8_t bytes[gizclaw_rpc_v1_ClientLuaAppInstallStreamRequest_size];
  pb_ostream_t output = pb_ostream_from_buffer(bytes, sizeof(bytes));
  assert(pb_encode(&output,
                   gizclaw_rpc_v1_ClientLuaAppInstallStreamRequest_fields,
                   &metadata));
  for (unsigned i = 0; i < 5000; ++i) {
    void *session = NULL;
    int rc = provider->begin(
        provider->userdata, 0,
        gzc_str_from_parts((const char *)bytes, output.bytes_written),
        &session);
    if (rc == GZC_OK) {
      assert(session);
      return session;
    }
    assert(rc == GZC_ERR_UNSUPPORTED && session == NULL);
    pause_worker();
  }
  assert(false);
  return NULL;
}
typedef struct reply {
  unsigned count;
  bool error;
  int code;
} reply_t;
static int respond(void *user, const gzc_rpc_provider_response_t *response) {
  reply_t *reply = user;
  ++reply->count;
  reply->error = response->has_error;
  reply->code = response->error_code;
  if (!response->has_error) {
    gizclaw_rpc_v1_ClientLuaAppInstallResponse value = {0};
    pb_istream_t input =
        pb_istream_from_buffer(response->payload, response->payload_len);
    assert(pb_decode(&input, gizclaw_rpc_v1_ClientLuaAppInstallResponse_fields,
                     &value));
    assert(value.has_app && !strcmp(value.app.app_id, "demo") &&
           !strcmp(value.app.version, "1.0.0"));
  }
  return GZC_OK;
}
static reply_t upload(const gzc_rpc_stream_provider_t *provider, void *session,
                      const uint8_t *archive, size_t size) {
  uint8_t bytes[32768];
  for (size_t at = 0; at < size;) {
    size_t take = size - at;
    if (take > sizeof(bytes))
      take = sizeof(bytes);
    memcpy(bytes, archive + at, take);
    int rc = GZC_ERR_WOULD_BLOCK;
    for (unsigned retry = 0; retry < 5000 && rc == GZC_ERR_WOULD_BLOCK;
         ++retry) {
      rc = provider->write(session, bytes, take);
      if (rc == GZC_ERR_WOULD_BLOCK)
        pause_worker();
    }
    assert(rc == GZC_OK);
    memset(bytes, 0xff,
           take); /* SDK's borrowed frame may be reused immediately. */
    at += take;
  }
  reply_t reply = {0};
  int rc = GZC_ERR_WOULD_BLOCK;
  for (unsigned retry = 0; retry < 5000 && rc == GZC_ERR_WOULD_BLOCK; ++retry) {
    rc = provider->finish(session, respond, &reply);
    if (rc == GZC_ERR_WOULD_BLOCK)
      pause_worker();
  }
  assert(rc == GZC_OK && reply.count == 1);
  provider->close(session, GZC_OK);
  return reply;
}
static bool fail_join;
static int start(void *user, const h2_pal_task_options_t *options,
                 h2_pal_task_entry_t entry, void *ctx, h2_pal_task_t **out) {
  (void)user;
  return h2_pal_task_start(h2_desktop_platform_task_api(), options, entry, ctx,
                           out);
}
static int join(void *user, h2_pal_task_t *task) {
  (void)user;
  if (fail_join) {
    fail_join = false;
    return H2_PAL_ERR_IO;
  }
  return h2_pal_task_join(h2_desktop_platform_task_api(), task);
}
typedef struct launch {
  bool reserved, launched;
  h2_gizclaw_lua_app_lease_t *lease;
} launch_t;
static void completed(void *user, int result) {
  launch_t *launch = user;
  assert(launch->reserved);
  launch->reserved = false;
  launch->launched = result == H2_PAL_OK;
  // This test does not start a Lua user; completion is its final use.
  h2_gizclaw_lua_app_lease_release(&launch->lease);
}
static h2_pal_result_t prepare(void *user, const h2_gizclaw_lua_app_run_t *run,
                               h2_gizclaw_lua_app_handoff_t *out) {
  launch_t *launch = user;
  if (launch->reserved)
    return H2_PAL_ERR_BUSY;
  assert(!strcmp(run->app.app_id, "demo") &&
         !strcmp(run->app.version, "1.0.0"));
  launch->reserved = true;
  launch->lease = run->lease;
  *out = (h2_gizclaw_lua_app_handoff_t){completed, launch};
  return H2_PAL_OK;
}
int main(void) {
  assert(h2_atomic_bool_init(&gate_enabled, false) == H2_ATOMIC_OK);
  assert(h2_atomic_bool_init(&gate_entered, false) == H2_ATOMIC_OK);
  assert(h2_atomic_bool_init(&gate_released, false) == H2_ATOMIC_OK);
  h2_app_test_fs_t fs;
  assert(h2_app_test_fs_init(&fs, &mem) == 0);
  static const char initial_source[] = "assert(args.mode == \"single\")\n";
  put(&fs.api, "/preinstalled/demo.lua", initial_source,
      sizeof(initial_source) - 1u);
  put(&fs.api, "/preinstalled/index.json", fixture_preinstalled_index,
      sizeof(fixture_preinstalled_index));
  h2_gizclaw_lua_app_catalog_config_t directory = {
      .allocator = &mem,
      .fs = &fs.api,
      .sync = h2_desktop_platform_sync_api(),
      .preinstalled_index = "/preinstalled/index.json",
      .install_root = "/apps"};
  h2_gizclaw_lua_app_catalog_t *apps = NULL;
  assert(h2_gizclaw_lua_app_catalog_create(&directory, &apps) == 0);
  h2_pal_fs_vtable_t fs_vtable = *fs.api.vtable;
  base_write = fs_vtable.write;
  fs_vtable.write = gated_write;
  fs.api.vtable = &fs_vtable;
  const h2_pal_task_vtable_t task_vtable = {start, join};
  const h2_pal_task_api_t task = {.vtable = &task_vtable};
  const h2_gizclaw_vtable_t vtable = {.prepare_lua_app_run = prepare};
  launch_t launch = {0};
  h2_gizclaw_config_t config = {
      .allocator = &mem, .lua_apps = apps, .vtable = &vtable, .user = &launch};
  h2_gizclaw_lua_stream_t *stream = NULL;
  config.time = &progress_time;
  assert(h2_gizclaw_lua_stream_create_internal(&config, &task, directory.sync,
                                               &stream) == 0);
  assert(h2_gizclaw_lua_stream_start_internal(stream) == 0);
  const gzc_rpc_stream_provider_t *provider =
      h2_gizclaw_lua_stream_provider_internal(stream);
  assert(provider && sizeof(fixture_stream) > 65535u);
  /* Progress requires the poll owner's WOULD_BLOCK path to yield. */
  h2_atomic_store(&gate_enabled, true);
  void *session =
      open_stream(provider, sizeof(fixture_stream), fixture_stream_sha256);
  assert(provider->write(session, fixture_stream, 32768u) == GZC_OK);
  for (unsigned retry = 0; retry < 5000 && !h2_atomic_load(&gate_entered);
       ++retry)
    pause_worker();
  assert(h2_atomic_load(&gate_entered));
  assert(provider->write(session, fixture_stream + 32768u, 1u) ==
         GZC_ERR_WOULD_BLOCK);
  assert(yields > 0 && h2_atomic_load(&gate_released));
  provider->close(session, GZC_ERR_CLOSED);
  wait_version(apps, "0.1.0");
  session =
      open_stream(provider, sizeof(fixture_stream), fixture_stream_sha256);
  reply_t reply =
      upload(provider, session, fixture_stream, sizeof(fixture_stream));
  assert(!reply.error);
  wait_version(apps, "1.0.0");
  session = open_stream(provider, sizeof(fixture_update), fixture_valid_sha256);
  assert(provider->write(session, fixture_update, 3) == GZC_OK);
  provider->close(session, GZC_ERR_CLOSED);
  wait_version(apps, "1.0.0");
  session = open_stream(
      provider, sizeof(fixture_valid),
      "0000000000000000000000000000000000000000000000000000000000000000");
  reply = upload(provider, session, fixture_valid, sizeof(fixture_valid));
  assert(reply.error && reply.code == H2_GIZCLAW_RPC_ERROR_INVALID_ARGUMENT);
  wait_version(apps, "1.0.0");
  h2_gizclaw_lua_apps_t *rpc = NULL;
  assert(h2_gizclaw_lua_apps_create_internal(&config, &rpc) == 0);
  const uint8_t run[] = {0x0a, 4, 'd', 'e', 'm', 'o'};
  h2_gizclaw_rpc_provider_response_t response = {0};
  assert(h2_gizclaw_lua_apps_rpc_internal(
             rpc, H2_GIZCLAW_TOOL_LUA_APP_RUN,
             (h2_gizclaw_rpc_bytes_t){run, sizeof(run)}, &response) == 0);
  assert(launch.reserved && !launch.launched && response.on_complete);
  h2_gizclaw_rpc_provider_response_t busy = {0};
  assert(h2_gizclaw_lua_apps_rpc_internal(
             rpc, H2_GIZCLAW_TOOL_LUA_APP_RUN,
             (h2_gizclaw_rpc_bytes_t){run, sizeof(run)},
             &busy) == H2_PAL_ERR_BUSY);
  response.on_complete(response.complete_user, H2_PAL_ERR_CLOSED);
  assert(!launch.reserved && !launch.launched);
  assert(h2_gizclaw_lua_apps_rpc_internal(
             rpc, H2_GIZCLAW_TOOL_LUA_APP_RUN,
             (h2_gizclaw_rpc_bytes_t){run, sizeof(run)}, &response) == 0);
  response.on_complete(response.complete_user, H2_PAL_OK);
  assert(!launch.reserved && launch.launched);
  const uint8_t invalid[] = {0x0a, 1, 0xff};
  assert(h2_gizclaw_lua_apps_rpc_internal(
             rpc, H2_GIZCLAW_TOOL_LUA_APP_RUN,
             (h2_gizclaw_rpc_bytes_t){invalid, sizeof(invalid)},
             &response) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_lua_apps_rpc_internal(
             rpc, H2_GIZCLAW_TOOL_LUA_APP_UNINSTALL,
             (h2_gizclaw_rpc_bytes_t){invalid, sizeof(invalid)},
             &response) == H2_PAL_ERR_INVALID_ARG);
  const uint8_t nul_id[] = {0x0a, 5, 'd', 'e', 'm', 'o', 0};
  assert(h2_gizclaw_lua_apps_rpc_internal(
             rpc, H2_GIZCLAW_TOOL_LUA_APP_UNINSTALL,
             (h2_gizclaw_rpc_bytes_t){nul_id, sizeof(nul_id)},
             &response) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_lua_apps_rpc_internal(
             rpc, H2_GIZCLAW_TOOL_LUA_APP_UNINSTALL,
             (h2_gizclaw_rpc_bytes_t){run, sizeof(run)}, &response) == 0);
  assert(h2_gizclaw_lua_apps_rpc_internal(
             rpc, H2_GIZCLAW_TOOL_LUA_APP_UNINSTALL,
             (h2_gizclaw_rpc_bytes_t){run, sizeof(run)}, &response) == 0);
  assert(h2_gizclaw_lua_apps_rpc_internal(
             rpc, H2_GIZCLAW_TOOL_LUA_APP_RUN,
             (h2_gizclaw_rpc_bytes_t){run, sizeof(run)},
             &response) == H2_PAL_ERR_NOT_FOUND);
  h2_gizclaw_lua_apps_destroy_internal(rpc);
  fail_join = true;
  assert(h2_gizclaw_lua_stream_stop_internal(stream) == H2_PAL_ERR_IO);
  assert(h2_gizclaw_lua_stream_stop_internal(stream) == 0);
  h2_gizclaw_lua_stream_destroy_internal(stream);
  assert(h2_gizclaw_lua_app_catalog_destroy(apps) == 0);
  assert(fs.active_handles == 0 && h2_app_test_fs_deinit(&fs) == 0);
  h2_atomic_bool_destroy(&gate_released);
  h2_atomic_bool_destroy(&gate_entered);
  h2_atomic_bool_destroy(&gate_enabled);
  return 0;
}
