#include "h2_gizclaw_e2e_device_http.h"
#include "h2_gizclaw_lua_app_catalog.h"
#include "h2_lua_job.h"
#include "h2_sha256.h"
#include "h2_encoding.h"
#include <stdio.h>
#include <string.h>

typedef struct h2_gizclaw_e2e_lua_state {
  h2_gizclaw_e2e_fixture_t *fixture;
  h2_gizclaw_lua_app_catalog_t *apps;
  h2_lua_host_t *host;
  h2_lua_job_id_t job;
  h2_gizclaw_lua_app_run_t request;
  char params[H2_GIZCLAW_LUA_APP_PARAMS_BYTES_MAX +
              2u * H2_GIZCLAW_LUA_APP_PARAMS_MAX];
  char root[80];
  char first_frame_sha[65];
  size_t width, height;
  h2_atomic_int_t pending;
  h2_atomic_uint_t frames;
  bool pending_initialized, frames_initialized, owns_root;
} lua_state_t;

static const char preinstalled_source[] =
    "assert(args.message == 'golden')\n"
    "local kv = require('kv')\n"
    "assert(kv.set('golden', args.message))\n"
    "assert(kv.get('golden') == args.message)\n"
    "return args.message\n";

static int path(char *out, size_t capacity, const char *root,
                const char *name) {
  int n = snprintf(out, capacity, "%s/%s", root, name);
  return n >= 0 && (size_t)n < capacity ? H2_PAL_OK : H2_PAL_ERR_NO_SPACE;
}
static int write_fixture(lua_state_t *s, const char *name, const char *bytes,
                         size_t size) {
  char location[192];
  int rc = path(location, sizeof(location), s->root, name);
  if (rc != H2_PAL_OK)
    return rc;
  for (char *p = location + 1; *p; ++p) {
    if (*p != '/')
      continue;
    *p = 0;
    h2_pal_fs_stat_t stat = {0};
    rc = h2_pal_fs_stat(s->fixture->runtime->fs, location, &stat);
    if (rc == H2_PAL_ERR_NOT_FOUND)
      rc = h2_pal_fs_mkdir(s->fixture->runtime->fs, location);
    else if (rc == H2_PAL_OK && !stat.is_dir)
      rc = H2_PAL_ERR_FORMAT;
    *p = '/';
    if (rc != H2_PAL_OK)
      return rc;
  }
  h2_pal_fs_file_t *file = NULL;
  rc = h2_pal_fs_open(s->fixture->runtime->fs, location,
                      H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
  size_t at = 0;
  while (rc == H2_PAL_OK && at < size) {
    size_t written = 0;
    rc = h2_pal_fs_write(s->fixture->runtime->fs, file, bytes + at, size - at,
                         &written);
    if (rc == H2_PAL_OK && (!written || written > size - at))
      rc = H2_PAL_ERR_IO;
    if (rc == H2_PAL_OK)
      at += written;
  }
  if (rc == H2_PAL_OK)
    rc = h2_pal_fs_sync(s->fixture->runtime->fs, file);
  if (file) {
    int close = h2_pal_fs_close(s->fixture->runtime->fs, file);
    if (rc == H2_PAL_OK)
      rc = close;
  }
  return rc;
}
static int digest_file(lua_state_t *s, const char *location, char out[65],
                       size_t *size) {
  h2_sha256_t sha;
  h2_sha256_init(&sha);
  *size = 0;
  h2_pal_fs_file_t *file = NULL;
  int rc = h2_pal_fs_open(s->fixture->runtime->fs, location,
                          H2_PAL_FS_OPEN_READ, &file);
  uint8_t buffer[4096];
  while (rc == H2_PAL_OK) {
    size_t got = 0;
    rc = h2_pal_fs_read(s->fixture->runtime->fs, file, buffer, sizeof(buffer),
                        &got);
    if (rc != H2_PAL_OK || !got)
      break;
    if (got > sizeof(buffer)) {
      rc = H2_PAL_ERR_IO;
      break;
    }
    h2_sha256_update(&sha, buffer, got);
    *size += got;
  }
  if (file) {
    int close = h2_pal_fs_close(s->fixture->runtime->fs, file);
    if (rc == H2_PAL_OK)
      rc = close;
  }
  uint8_t digest[32];
  h2_sha256_finish(&sha, digest);
  h2_sha256_hex(digest, out);
  return rc;
}

static void handoff_complete(void *user, int result) {
  lua_state_t *s = user;
  h2_atomic_store_explicit(&s->pending, result == H2_PAL_OK ? 1 : 0,
                           H2_ATOMIC_RELEASE);
}
static h2_pal_result_t prepare_run(void *user,
                                   const h2_gizclaw_lua_app_run_t *request,
                                   h2_gizclaw_lua_app_handoff_t *out) {
  h2_gizclaw_e2e_fixture_t *fixture = user;
  lua_state_t *s = fixture->lua_state;
  int expected = 0;
  if (!h2_atomic_int_compare_exchange(&s->pending, &expected, -1,
                                      H2_ATOMIC_ACQ_REL, H2_ATOMIC_ACQUIRE))
    return H2_PAL_ERR_BUSY;
  s->request = *request;
  size_t at = 0;
  for (size_t i = 0; i < request->param_count; ++i) {
    const char *values[] = {request->params[i].name, request->params[i].value};
    for (size_t j = 0; j < 2; ++j) {
      size_t len = strlen(values[j]) + 1u;
      if (len > sizeof(s->params) - at) {
        h2_atomic_store(&s->pending, 0);
        return H2_PAL_ERR_NO_SPACE;
      }
      memcpy(s->params + at, values[j], len);
      if (j == 0)
        s->request.params[i].name = s->params + at;
      else
        s->request.params[i].value = s->params + at;
      at += len;
    }
  }
  *out = (h2_gizclaw_lua_app_handoff_t){handoff_complete, s};
  return H2_PAL_OK;
}
static const h2_gizclaw_vtable_t lua_vtable = {.prepare_lua_app_run =
                                                   prepare_run};

static void presented(void *user, h2_lua_job_id_t job, const uint16_t *pixels,
                      size_t width, size_t height) {
  (void)job;
  lua_state_t *s = user;
  if (h2_atomic_load(&s->frames) == 0) {
    h2_sha256_t sha;
    uint8_t digest[32];
    h2_sha256_init(&sha);
    h2_sha256_update(&sha, (const uint8_t *)pixels,
                     width * height * sizeof(*pixels));
    h2_sha256_finish(&sha, digest);
    h2_sha256_hex(digest, s->first_frame_sha);
    s->width = width;
    s->height = height;
  }
  h2_atomic_fetch_add_explicit(&s->frames, 1u, H2_ATOMIC_RELEASE);
}

static int stop_host(lua_state_t *s) {
  if (!s->host)
    return H2_PAL_OK;
  int rc = h2_lua_host_stop(s->host);
  if (rc == H2_PAL_OK) {
    for (unsigned i = 0; i < 500; ++i) {
      rc = h2_lua_host_join(s->host);
      if (rc != H2_PAL_ERR_BUSY)
        break;
      (void)h2_pal_time_sleep_ms(s->fixture->time, 10);
    }
  }
  if (rc == H2_PAL_OK)
    rc = h2_lua_host_destroy_checked(s->host);
  if (rc == H2_PAL_OK) {
    s->host = NULL;
    s->job = H2_LUA_JOB_ID_NONE;
    h2_atomic_store(&s->pending, 0);
  }
  return rc;
}

static int run_prepared(lua_state_t *s, bool game) {
  const h2_gizclaw_e2e_config_t *c = s->fixture->config;
  for (unsigned i = 0;
       i < 300 && h2_atomic_load_explicit(&s->pending, H2_ATOMIC_ACQUIRE) != 1;
       ++i)
    (void)h2_pal_time_sleep_ms(s->fixture->time, 10);
  if (h2_atomic_load(&s->pending) != 1)
    return H2_PAL_ERR_TIMEOUT;
  char kv_root[96];
  int rc = path(kv_root, sizeof(kv_root), s->root, "kv");
  h2_atomic_store(&s->frames, 0u);
  h2_lua_host_config_t config = {
      .runtime = s->fixture->runtime,
      .allocator = s->fixture->allocator,
      .worker_count = 1,
      .max_jobs = 1,
      .worker_stack_size = 64u * 1024u,
      .display_worker_stack_size = 32768u,
      .vm_memory_limit_bytes = 2u * 1024u * 1024u,
      .source_limit_bytes = 256u * 1024u,
      .vm_heap_bytes = game ? 2u * 1024u * 1024u : 0,
      .output_limit_bytes = 4096u,
      .callback_capacity_per_job = 16,
      .event_delivery_capacity = 32,
      .execution_timeout_ms = 90000,
      .storage = {.fs = s->fixture->runtime->fs, .root = kv_root},
      .on_display_present = presented,
      .display_present_user = s};
  if (rc == H2_PAL_OK)
    rc = h2_lua_host_create(&config, &s->host);
  if (rc == H2_PAL_OK)
    rc = h2_lua_host_start(s->host);
  h2_lua_arg_t args[H2_GIZCLAW_LUA_APP_PARAMS_MAX];
  for (size_t i = 0; i < s->request.param_count; ++i)
    args[i] =
        (h2_lua_arg_t){s->request.params[i].name, s->request.params[i].value};
  char chunk[H2_GIZCLAW_LUA_APP_PATH_MAX + 1];
  (void)snprintf(chunk, sizeof(chunk), "@%s", s->request.app.entry);
  if (rc == H2_PAL_OK)
    rc = h2_lua_job_submit_path(s->host, s->request.app.app_id, chunk,
                                s->request.app.entry, args,
                                s->request.param_count, &s->job);
  h2_lua_job_status_t status = {0};
  bool completed = false;
  for (unsigned i = 0; rc == H2_PAL_OK && i < 1800; ++i) {
    rc = h2_lua_job_get_status(s->host, s->job, &status);
    unsigned frames = h2_atomic_load_explicit(&s->frames, H2_ATOMIC_ACQUIRE);
    if (rc != H2_PAL_OK)
      break;
    if (status.state >= H2_LUA_JOB_FAILED) {
      rc = H2_PAL_ERR_INVALID_STATE;
      break;
    }
    if ((!game && status.state == H2_LUA_JOB_SUCCEEDED) ||
        (game && frames >= 3u)) {
      completed = true;
      break;
    }
    (void)h2_pal_time_sleep_ms(s->fixture->time, 50);
  }
  if (rc == H2_PAL_OK && !completed)
    rc = H2_PAL_ERR_TIMEOUT;
  if (rc == H2_PAL_OK && !game) {
    char result[32];
    size_t size = 0;
    int has_result = 0;
    rc = h2_lua_job_get_result(s->host, s->job, result, sizeof(result), &size,
                               &has_result);
    if (rc == H2_PAL_OK && (!has_result || strcmp(result, "golden")))
      rc = H2_PAL_ERR_FORMAT;
  }
  if (game && h2_atomic_load_explicit(&s->frames, H2_ATOMIC_ACQUIRE)) {
    h2_gizclaw_e2e_emit("H2_GIZCLAW_E2E stage=lua-golden frame_sha256=%s "
                        "width=%zu height=%zu frames=%u "
                        "vm_bytes=%zu state=%d record=%d\n",
                        s->first_frame_sha, s->width, s->height,
                        h2_atomic_load(&s->frames), status.memory_used,
                        (int)status.state, c->lua_record_golden);
    if (rc == H2_PAL_OK && !c->lua_record_golden &&
        strcmp(s->first_frame_sha, c->lua_frame_sha256))
      rc = H2_PAL_ERR_FORMAT;
  }
  if (rc != H2_PAL_OK) {
    char message[sizeof(status.message)];
    memcpy(message, status.message, sizeof(message));
    message[sizeof(message) - 1u] = 0;
    for (char *p = message; *p; ++p)
      if (*p == '\r' || *p == '\n')
        *p = ' ';
    h2_gizclaw_e2e_emit(
        "H2_GIZCLAW_E2E stage=lua-golden phase=run rc=%d state=%d message=%s\n",
        rc, (int)status.state, message);
  }
  int cleanup = stop_host(s);
  return rc == H2_PAL_OK ? cleanup : rc;
}

static int dispose(h2_gizclaw_e2e_fixture_t *fixture) {
  lua_state_t *s = fixture->lua_state;
  if (!s)
    return H2_PAL_OK;
  int rc = stop_host(s);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_lua_app_catalog_destroy(s->apps);
  if (rc != H2_PAL_OK)
    return rc;
  s->apps = NULL;
  fixture->device_lua_apps = NULL;
  if (s->owns_root && !fixture->config->lua_preserve_storage) {
    rc = h2_pal_fs_clear(fixture->runtime->fs, s->root);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND)
      return rc;
  }
  if (s->owns_root && fixture->config->lua_preserve_storage)
    h2_gizclaw_e2e_emit("H2_GIZCLAW_E2E stage=lua-golden phase=storage "
                        "action=preserved root=%s\n",
                        s->root);
  if (s->pending_initialized)
    h2_atomic_int_destroy(&s->pending);
  if (s->frames_initialized)
    h2_atomic_uint_destroy(&s->frames);
  h2_pal_mem_free(fixture->allocator, s);
  fixture->lua_state = NULL;
  fixture->device_cleanup = NULL;
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_prepare_lua_apps(h2_gizclaw_e2e_fixture_t *fixture) {
  const h2_gizclaw_e2e_config_t *c = fixture->config;
  const char *root =
      c->lua_storage_root ? c->lua_storage_root : "/data/gizclaw-e2e-lua";
  if (strncmp(root, "/data/", 6) || strlen(root) >= 80 ||
      strlen(root) < strlen("gizclaw-e2e-lua") ||
      strcmp(root + strlen(root) - strlen("gizclaw-e2e-lua"),
             "gizclaw-e2e-lua") ||
      strspn(root, "/abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ012345"
                   "6789_-") != strlen(root) ||
      strstr(root, "//"))
    return H2_PAL_ERR_INVALID_ARG;
  if (!fixture->runtime->fs || !c->device_api_url || !c->lua_app_url ||
      !c->lua_app_http_url || !c->lua_app_https_url ||
      !h2_gizclaw_lua_app_id_valid(c->lua_app_id) || !c->lua_archive_sha256 ||
      strlen(c->lua_archive_sha256) != 64 || !c->lua_entry_sha256 ||
      strlen(c->lua_entry_sha256) != 64 ||
      (!c->lua_record_golden &&
       (!c->lua_frame_sha256 || strlen(c->lua_frame_sha256) != 64)))
    return H2_PAL_ERR_INVALID_ARG;
  lua_state_t *s = h2_pal_mem_alloc(fixture->allocator, sizeof(*s));
  if (!s)
    return H2_PAL_ERR_NO_MEMORY;
  memset(s, 0, sizeof(*s));
  s->fixture = fixture;
  fixture->lua_state = s;
  fixture->device_cleanup = dispose;
  if (h2_atomic_int_init(&s->pending, 0) != H2_ATOMIC_OK)
    return H2_PAL_ERR_NO_MEMORY;
  s->pending_initialized = true;
  if (h2_atomic_uint_init(&s->frames, 0) != H2_ATOMIC_OK)
    return H2_PAL_ERR_NO_MEMORY;
  s->frames_initialized = true;
  strcpy(s->root, root);
  static const char owner[] = "gizos-lua-e2e-v1\n";
  h2_pal_fs_stat_t stat = {0};
  int rc = h2_pal_fs_stat(fixture->runtime->fs, root, &stat);
  if (rc == H2_PAL_OK) {
    char location[192], bytes[sizeof(owner)];
    h2_pal_fs_file_t *file = NULL;
    size_t got = 0;
    rc = path(location, sizeof(location), root, ".owner");
    if (rc == H2_PAL_OK)
      rc = h2_pal_fs_open(fixture->runtime->fs, location, H2_PAL_FS_OPEN_READ,
                          &file);
    if (rc == H2_PAL_OK)
      rc = h2_pal_fs_read(fixture->runtime->fs, file, bytes, sizeof(bytes),
                          &got);
    if (file) {
      int close_rc = h2_pal_fs_close(fixture->runtime->fs, file);
      if (rc == H2_PAL_OK)
        rc = close_rc;
    }
    if (rc == H2_PAL_OK &&
        (got != sizeof(owner) - 1u || memcmp(bytes, owner, got)))
      rc = H2_PAL_ERR_INVALID_STATE;
    if (rc != H2_PAL_OK)
      return rc;
    s->owns_root = true;
  } else if (rc == H2_PAL_ERR_NOT_FOUND) {
    s->owns_root = true;
    rc = write_fixture(s, ".owner", owner, sizeof(owner) - 1u);
  }
  if (rc == H2_PAL_OK)
    rc = write_fixture(s, "preinstalled/e2e-preinstalled.lua",
                       preinstalled_source, sizeof(preinstalled_source) - 1u);
  char index[2048], hash[65], directory[192] = {0}, index_path[192] = {0},
                              installed[192] = {0};
  uint8_t digest[32];
  h2_sha256_t sha;
  h2_sha256_init(&sha);
  h2_sha256_update(&sha, (const uint8_t *)preinstalled_source,
                   sizeof(preinstalled_source) - 1u);
  h2_sha256_finish(&sha, digest);
  h2_sha256_hex(digest, hash);
  if (rc == H2_PAL_OK)
    rc = path(directory, sizeof(directory), root, "preinstalled");
  int n = snprintf(
      index, sizeof(index),
      "{\"format\":1,\"apps\":[{\"root\":\"%s\",\"manifest\":{\"format\":1,"
      "\"type\":\"lua-app\",\"app_id\":\"e2e-preinstalled\",\"version\":\"0.1."
      "0\",\"entry\":\"e2e-preinstalled.lua\",\"data_dir\":null,\"files\":[{"
      "\"path\":\"e2e-preinstalled.lua\",\"size\":%zu,\"sha256\":\"%s\"}]}}]}",
      directory, sizeof(preinstalled_source) - 1u, hash);
  if (rc == H2_PAL_OK && (n < 0 || (size_t)n >= sizeof(index)))
    rc = H2_PAL_ERR_NO_SPACE;
  if (rc == H2_PAL_OK)
    rc = write_fixture(s, "preinstalled.json", index, (size_t)n);
  if (rc == H2_PAL_OK)
    rc = path(index_path, sizeof(index_path), root, "preinstalled.json");
  if (rc == H2_PAL_OK)
    rc = path(installed, sizeof(installed), root, "installed");
  h2_gizclaw_lua_app_catalog_config_t config = {.allocator = fixture->allocator,
                                                .fs = fixture->runtime->fs,
                                                .sync = fixture->runtime->sync,
                                                .preinstalled_index =
                                                    index_path,
                                                .install_root = installed};
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_lua_app_catalog_create(&config, &s->apps);
  fixture->device_lua_apps = s->apps;
  fixture->device_vtable = &lua_vtable;
  return rc;
}

static int verify_catalog(h2_gizclaw_e2e_api_t *api, const char *game,
                          bool installed) {
  int rc = h2_gizclaw_e2e_tool_call(api, "lua.app.list", "{}", 200);
  if (rc != H2_PAL_OK)
    return rc;
  h2_pal_json_value_t *array = h2_gizclaw_e2e_api_field(api, "result.apps");
  size_t count = 0;
  rc = h2_pal_json_array_size(api->json, array, &count);
  bool preinstalled = false, found = false;
  for (size_t i = 0; rc == H2_PAL_OK && i < count; ++i) {
    h2_pal_json_value_t *app = NULL, *id = NULL;
    h2_pal_json_string_view_t text = {0};
    rc = h2_pal_json_array_get(api->json, array, i, &app);
    if (rc == H2_PAL_OK)
      rc = h2_pal_json_object_get(api->json, app, "app_id", 6, &id);
    if (rc == H2_PAL_OK)
      rc = h2_pal_json_value_get_string(api->json, id, &text);
    if (rc == H2_PAL_OK) {
      preinstalled |= text.len == strlen("e2e-preinstalled") &&
                      !memcmp(text.data, "e2e-preinstalled", text.len);
      found |= text.len == strlen(game) && !memcmp(text.data, game, text.len);
    }
  }
  return rc == H2_PAL_OK && (!preinstalled || found != installed ||
                             count != (installed ? 2u : 1u))
             ? H2_PAL_ERR_FORMAT
             : rc;
}

/* The fixture config's Base64 carrier is decoded only by the controller.
 * PAL HTTP borrows this upload buffer; the device receives Binary frames. */
static int fixture_archive(lua_state_t *s, uint8_t **out, size_t *out_size) {
  *out = NULL;
  *out_size = 0;
  const char *uri = s->fixture->config->lua_app_url;
  static const char prefix[] = "data:application/zlib;base64,";
  if (strncmp(uri, prefix, sizeof(prefix) - 1u) ||
      strlen(uri) > 256u * 1024u)
    return H2_PAL_ERR_INVALID_ARG;
  const char *text = uri + sizeof(prefix) - 1u;
  size_t length = strlen(text), capacity = 0;
  if (!length || h2_encoding_max_decoded_len(&h2_encoding_base64_std, length,
                                             &capacity) != H2_ENCODING_OK)
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t *bytes = h2_pal_mem_alloc(s->fixture->allocator, capacity);
  if (!bytes)
    return H2_PAL_ERR_NO_MEMORY;
  if (h2_encoding_decode(&h2_encoding_base64_std, text, length, bytes, capacity,
                         out_size) != H2_ENCODING_OK) {
    h2_pal_mem_free(s->fixture->allocator, bytes);
    *out_size = 0;
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out = bytes;
  return H2_PAL_OK;
}

static int run_game(h2_gizclaw_e2e_api_t *api, lua_state_t *s) {
  char run[256];
  int n = snprintf(run, sizeof(run),
                     "{\"app_id\":\"%s\",\"params\":{\"lang\":\"en-US\","
                     "\"language\":\"en-US\"}}",
                     s->fixture->config->lua_app_id);
  if (n < 0 || (size_t)n >= sizeof(run))
    return H2_PAL_ERR_NO_SPACE;
  int rc = h2_gizclaw_e2e_tool_call(api, "lua.app.run", run, 200);
  return rc == H2_PAL_OK ? run_prepared(s, true) : rc;
}

static int verify_retained(h2_gizclaw_e2e_api_t *api, lua_state_t *s) {
  const h2_gizclaw_e2e_config_t *c = s->fixture->config;
  h2_gizclaw_lua_app_info_t app;
  int rc = h2_gizclaw_lua_app_catalog_get(s->apps, c->lua_app_id, &app);
  bool present = rc == H2_PAL_OK;
  char hash[65];
  size_t size = 0;
  if (rc == H2_PAL_ERR_NOT_FOUND && !c->lua_expect_persisted)
    rc = H2_PAL_OK;
  else if (rc == H2_PAL_OK) {
    rc = digest_file(s, app.entry, hash, &size);
    if (rc == H2_PAL_OK && strcmp(hash, c->lua_entry_sha256))
      rc = H2_PAL_ERR_FORMAT;
  }
  h2_gizclaw_e2e_emit(
      "H2_GIZCLAW_E2E stage=lua-golden source=persisted expected=%d "
      "present=%d entry_bytes=%zu sha256_match=%d rejected=%d rc=%d\n",
      c->lua_expect_persisted, present, size, present && rc == H2_PAL_OK,
      c->lua_expect_persisted && rc != H2_PAL_OK, rc);
  if (rc == H2_PAL_OK && present)
    rc = verify_catalog(api, c->lua_app_id, true);
  if (rc == H2_PAL_OK && present)
    rc = run_game(api, s);
  return rc;
}

int h2_gizclaw_e2e_run_lua_apps(h2_gizclaw_e2e_fixture_t *fixture) {
  lua_state_t *s = fixture->lua_state;
  const h2_gizclaw_e2e_config_t *c = fixture->config;
  h2_gizclaw_e2e_api_t api = {.fixture = fixture};
  int rc = h2_yyjson_json_create(fixture->allocator, &api.provider);
  if (rc != H2_PAL_OK)
    return rc;
  api.json = h2_yyjson_json_api(api.provider);
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  rc = h2_gizclaw_rpc_api_key_create(
      service, h2_gizclaw_e2e_str("gizos-lua-golden"), false, 15000, &api.key);
  if (rc == H2_PAL_OK)
    rc = verify_catalog(&api, c->lua_app_id, c->lua_expect_persisted);
  if (rc == H2_PAL_OK)
    rc = verify_retained(&api, s);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_e2e_tool_call(
        &api, "lua.app.run",
        "{\"app_id\":\"e2e-preinstalled\",\"params\":{\"message\":\"golden\"}}",
        200);
  if (rc == H2_PAL_OK)
    rc = run_prepared(s, false);
  uint8_t *archive = NULL;
  size_t archive_size = 0;
  if (rc == H2_PAL_OK)
    rc = fixture_archive(s, &archive, &archive_size);
  if (rc == H2_PAL_OK && archive_size <= 65535u)
    rc = H2_PAL_ERR_INVALID_ARG;
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_e2e_lua_upload(&api, archive, archive_size,
                                   c->lua_archive_sha256, 200);
  if (rc == H2_PAL_OK) {
    static const char wrong_sha[] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    rc = h2_gizclaw_e2e_lua_upload(&api, archive, archive_size, wrong_sha, 400);
    h2_gizclaw_lua_app_info_t retained;
    char hash[65];
    size_t size = 0;
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_lua_app_catalog_get(s->apps, c->lua_app_id, &retained);
    if (rc == H2_PAL_OK)
      rc = digest_file(s, retained.entry, hash, &size);
    if (rc == H2_PAL_OK && strcmp(hash, c->lua_entry_sha256))
      rc = H2_PAL_ERR_FORMAT;
    h2_gizclaw_e2e_emit("H2_GIZCLAW_E2E stage=lua-golden source=binary-bad-sha "
                        "previous_preserved=%d rc=%d\n",
                        rc == H2_PAL_OK, rc);
  }
  h2_pal_mem_free(fixture->allocator, archive);
  if (rc == H2_PAL_OK)
    rc = verify_catalog(&api, c->lua_app_id, true);
  if (archive_size)
    h2_gizclaw_e2e_emit(
        "H2_GIZCLAW_E2E stage=lua-golden source=binary archive_bytes=%zu "
        "multi_frame=%d rc=%d\n",
        archive_size, archive_size > 65535u, rc);
  const char *sources[] = {c->lua_app_http_url, c->lua_app_https_url};
  for (size_t i = 0; rc == H2_PAL_OK && i < 2; ++i) {
    size_t capacity = strlen(sources[i]) + 128u;
    char *args = h2_pal_mem_alloc(fixture->allocator, capacity);
    if (!args) {
      rc = H2_PAL_ERR_NO_MEMORY;
      break;
    }
    if (strpbrk(sources[i], "\"\\\r\n"))
      rc = H2_PAL_ERR_INVALID_ARG;
    int n = snprintf(args, capacity, "{\"url\":\"%s\",\"sha256\":\"%s\"}",
                     sources[i], c->lua_archive_sha256);
    if (n < 0 || (size_t)n >= capacity)
      rc = H2_PAL_ERR_NO_SPACE;
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_e2e_tool_call(&api, "lua.app.install", args, 200);
    h2_pal_mem_free(fixture->allocator, args);
    if (rc == H2_PAL_OK)
      rc = verify_catalog(&api, c->lua_app_id, true);
    h2_gizclaw_lua_app_info_t app;
    char hash[65];
    size_t size = 0;
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_lua_app_catalog_get(s->apps, c->lua_app_id, &app);
    if (rc == H2_PAL_OK)
      rc = digest_file(s, app.entry, hash, &size);
    if (rc == H2_PAL_OK && strcmp(hash, c->lua_entry_sha256))
      rc = H2_PAL_ERR_FORMAT;
    h2_gizclaw_e2e_emit(
        "H2_GIZCLAW_E2E stage=lua-golden source=%s uri_bytes=%zu "
        "entry_bytes=%zu sha256_match=%d rc=%d\n",
        i == 0 ? "http" : "https",
        strlen(sources[i]), size, rc == H2_PAL_OK, rc);
  }
  if (rc == H2_PAL_OK)
    rc = run_game(&api, s);
  if (rc == H2_PAL_OK)
    rc = verify_catalog(&api, c->lua_app_id, true);
  int first = rc;
  if (first == H2_PAL_OK) {
    h2_gizclaw_lua_app_info_t prior;
    first = h2_gizclaw_lua_app_catalog_get(s->apps, c->lua_app_id, &prior);
    char args[64];
    int n = snprintf(args, sizeof(args), "{\"app_id\":\"%s\"}", c->lua_app_id);
    if (n < 0 || (size_t)n >= sizeof(args))
      first = H2_PAL_ERR_NO_SPACE;
    for (unsigned attempt = 0; first == H2_PAL_OK && attempt < 2u; ++attempt)
      first = h2_gizclaw_e2e_tool_call(&api, "lua.app.uninstall", args, 200);
    if (first == H2_PAL_OK)
      first = verify_catalog(&api, c->lua_app_id, false);
    if (first == H2_PAL_OK)
      first = h2_gizclaw_e2e_tool_call(&api, "lua.app.run", args, 404);
    h2_pal_fs_stat_t stat = {0};
    if (first == H2_PAL_OK && h2_pal_fs_stat(fixture->runtime->fs, prior.entry,
                                             &stat) != H2_PAL_ERR_NOT_FOUND)
      first = H2_PAL_ERR_FORMAT;
    h2_gizclaw_e2e_emit("H2_GIZCLAW_E2E stage=lua-uninstall repeated=1 "
                        "catalog_absent=%d entry_reclaimed=%d rc=%d\n",
                        first == H2_PAL_OK, first == H2_PAL_OK, first);
    size_t capacity = strlen(c->lua_app_url) + 128u;
    char *install = h2_pal_mem_alloc(fixture->allocator, capacity);
    if (first == H2_PAL_OK && !install)
      first = H2_PAL_ERR_NO_MEMORY;
    if (first == H2_PAL_OK) {
      n = snprintf(install, capacity, "{\"url\":\"%s\",\"sha256\":\"%s\"}",
                   c->lua_app_url, c->lua_archive_sha256);
      if (n < 0 || (size_t)n >= capacity)
        first = H2_PAL_ERR_NO_SPACE;
    }
    if (first == H2_PAL_OK)
      first = h2_gizclaw_e2e_tool_call(&api, "lua.app.install", install, 200);
    h2_pal_mem_free(fixture->allocator, install);
    if (first == H2_PAL_OK)
      first = verify_catalog(&api, c->lua_app_id, true);
    if (first == H2_PAL_OK)
      first = run_game(&api, s);
  }
  if (api.key.name[0]) {
    int cleanup = h2_gizclaw_rpc_api_key_revoke(
        service, h2_gizclaw_e2e_str(api.key.name), 15000);
    if (first == H2_PAL_OK)
      first = cleanup;
  }
  (void)h2_pal_json_document_destroy(api.json, &api.document);
  h2_yyjson_json_destroy(&api.provider);
  memset(&api.key, 0, sizeof(api.key));
  return first;
}
