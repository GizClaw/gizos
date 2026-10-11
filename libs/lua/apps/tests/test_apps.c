#include "h2_lua_apps.h"
#include "h2_app_test_fs.h"
#include "h2_desktop_platform.h"
#include "fixtures.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *allocate(void *user, size_t size) {
  (void)user;
  assert(size <= 128u * 1024u);
  return malloc(size);
}
static void *resize(void *user, void *ptr, size_t size) {
  (void)user;
  assert(size <= 128u * 1024u);
  return realloc(ptr, size);
}
static void release(void *user, void *ptr) {
  (void)user;
  free(ptr);
}
static const h2_pal_mem_vtable_t mem_vtable = {allocate, resize, release};
static const h2_pal_mem_api_t allocator = {.vtable = &mem_vtable};
static void *storage_allocate(void *user, size_t size) {
  (void)user;
  return malloc(size);
}
static void *storage_resize(void *user, void *ptr, size_t size) {
  (void)user;
  return realloc(ptr, size);
}
static const h2_pal_mem_vtable_t storage_vtable = {storage_allocate,
                                                   storage_resize, release};
static const h2_pal_mem_api_t storage = {.vtable = &storage_vtable};
#define URI(s) ((h2_lua_app_source_t){s, sizeof(s) - 1u})
static const unsigned char source[] = "assert(args.mode == \"single\")\n";
typedef struct http {
  const unsigned char *archive;
  size_t size, chunk, chunks, cancel_after;
  const char *location;
} http_t;
static int request(void *user, const h2_pal_http_request_t *r,
                   h2_pal_http_response_t *out) {
  http_t *h = user;
  assert(r->read_cb && r->chunk_buf && r->chunk_buf_cap <= 4096 &&
         !r->response_buf && !r->response_allocator);
  assert(r->retry_count == 0);
  out->status_code = 200;
  out->content_length = (int64_t)h->size;
  if (h->location) {
    int rc = h2_pal_http_deliver_response_header(r, "Location", 8, h->location,
                                                 strlen(h->location));
    if (rc != H2_PAL_OK)
      return rc;
  }
  for (size_t offset = 0; offset < h->size;) {
    if (h2_pal_http_request_is_canceled(r))
      return H2_PAL_ERR_CLOSED;
    size_t take = h->size - offset;
    if (take > h->chunk)
      take = h->chunk;
    ++h->chunks;
    int rc = r->read_cb(r->user, r, h->archive + offset, take, offset + take,
                        h->size - offset - take);
    if (rc != H2_PAL_OK)
      return rc;
    offset += take;
  }
  int eof_rc = r->read_cb(r->user, r, NULL, 0, h->size, 0);
  if (eof_rc != H2_PAL_OK)
    return eof_rc;
  out->body_len = h->size;
  return H2_PAL_OK;
}
static const h2_pal_http_vtable_t http_vtable = {.request = request};
static int cancelled(void *user) {
  http_t *h = user;
  return h->cancel_after && h->chunks >= h->cancel_after;
}
static void put(const h2_pal_fs_api_t *fs, const char *path, const void *bytes,
                size_t size) {
  h2_pal_fs_file_t *f = NULL;
  size_t written = 0;
  assert(h2_pal_fs_open(fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &f) == 0);
  assert(h2_pal_fs_write(fs, f, bytes, size, &written) == 0 && written == size);
  assert(h2_pal_fs_close(fs, f) == 0);
}
static void expect_file(const h2_pal_fs_api_t *fs, const char *path,
                        const void *bytes, size_t size) {
  h2_pal_fs_file_t *f = NULL;
  size_t got = 0;
  unsigned char buffer[128];
  assert(size <= sizeof(buffer));
  assert(h2_pal_fs_open(fs, path, H2_PAL_FS_OPEN_READ, &f) == 0);
  assert(h2_pal_fs_read(fs, f, buffer, sizeof(buffer), &got) == 0 &&
         got == size && !memcmp(bytes, buffer, size));
  assert(h2_pal_fs_close(fs, f) == 0);
}
static void check_version(h2_lua_apps_t *apps, const char *version) {
  h2_lua_app_info_t info[2];
  size_t count = 0;
  assert(h2_lua_apps_list(apps, info, 2, &count) == 0 && count == 1);
  assert(!strcmp(info[0].app_id, "demo") && !strcmp(info[0].version, version));
  h2_lua_app_info_t found;
  assert(h2_lua_apps_get(apps, "demo", &found) == 0 &&
         !strcmp(found.entry, info[0].entry));
}

static const h2_pal_fs_vtable_t *base_fs;
static void normalize(const char *input, char out[H2_LUA_APP_PATH_MAX]) {
  size_t at = 0;
  for (size_t i = 0;; ++i) {
    if (!input[i] || input[i] == '/')
      while (at && out[at - 1u] == '.')
        --at;
    assert(at < H2_LUA_APP_PATH_MAX);
    out[at++] = input[i];
    if (!input[i])
      break;
  }
}
static int alias_mkdir(void *user, const char *path) {
  char normalized[H2_LUA_APP_PATH_MAX];
  normalize(path, normalized);
  return base_fs->mkdir(user, normalized);
}
static int alias_open(void *user, const char *path, h2_pal_fs_open_mode_t mode,
                       h2_pal_fs_file_t **out) {
  char normalized[H2_LUA_APP_PATH_MAX];
  normalize(path, normalized);
  return base_fs->open(user, normalized, mode, out);
}
static int alias_stat(void *user, const char *path, h2_pal_fs_stat_t *out) {
  char normalized[H2_LUA_APP_PATH_MAX];
  normalize(path, normalized);
  return base_fs->stat(user, normalized, out);
}
static int alias_remove(void *user, const char *path) {
  char normalized[H2_LUA_APP_PATH_MAX];
  normalize(path, normalized);
  return base_fs->remove(user, normalized);
}
static int alias_rename(void *user, const char *from, const char *to) {
  char normalized_from[H2_LUA_APP_PATH_MAX], normalized_to[H2_LUA_APP_PATH_MAX];
  normalize(from, normalized_from);
  normalize(to, normalized_to);
  return base_fs->rename(user, normalized_from, normalized_to);
}
static void check_directory_alias(const h2_pal_sync_api_t *sync) {
  h2_app_test_fs_t fs;
  assert(h2_app_test_fs_init(&fs, &storage) == 0);
  base_fs = fs.api.vtable;
  h2_pal_fs_vtable_t aliases = *base_fs;
  aliases.mkdir = alias_mkdir;
  aliases.open = alias_open;
  aliases.stat = alias_stat;
  aliases.remove = alias_remove;
  aliases.rename = alias_rename;
  fs.api.vtable = &aliases;
  h2_lua_apps_config_t config = {.allocator = &allocator,
                                 .fs = &fs.api,
                                 .sync = sync,
                                 .install_root = "/apps"};
  h2_lua_apps_t *apps = NULL;
  h2_lua_app_info_t info, original;
  assert(h2_lua_apps_create(&config, &apps) == 0);
  http_t h = {.archive = fixture_valid,
              .size = sizeof(fixture_valid), .chunk = 7};
  h2_pal_http_api_t http = {.user = &h, .vtable = &http_vtable};
  for (unsigned generation = 0; generation < 2u; ++generation) {
    h = (http_t){.archive = generation ? fixture_update : fixture_valid,
                  .size = generation ? sizeof(fixture_update)
                                     : sizeof(fixture_valid), .chunk = 7};
    assert(h2_lua_apps_install(apps, &http, URI("https://example.test/app"),
                               NULL, 1000, NULL, NULL, &original) == 0);
    h = (http_t){.archive = fixture_alias_id,
                  .size = sizeof(fixture_alias_id), .chunk = 7};
    assert(h2_lua_apps_install(apps, &http, URI("https://example.test/app"),
                               NULL, 1000, NULL, NULL, &info) ==
           H2_PAL_ERR_FORMAT);
    check_version(apps, generation ? "2.0.0" : "1.0.0");
    expect_file(&fs.api, original.entry, source, sizeof(source) - 1u);
  }
  assert(h2_lua_apps_destroy(apps) == 0);
  assert(h2_lua_apps_create(&config, &apps) == 0);
  check_version(apps, "2.0.0");
  expect_file(&fs.api, original.entry, source, sizeof(source) - 1u);
  assert(h2_lua_apps_destroy(apps) == 0);
  h2_app_test_fs_deinit(&fs);
}
int main(void) {
  const h2_pal_mem_api_t *mem = &allocator;
  const h2_pal_sync_api_t *sync = h2_desktop_platform_sync_api();
  check_directory_alias(sync);
  h2_app_test_fs_t fs;
  /* Backend storage of the extracted file is independent of installer
   * scratch. The installer must never allocate this archive in one buffer. */
  assert(h2_app_test_fs_init(&fs, &storage) == 0);
  put(&fs.api, "/preinstalled/demo.lua", source, sizeof(source) - 1);
  put(&fs.api, "/preinstalled/index.json", fixture_preinstalled_index,
      sizeof(fixture_preinstalled_index));
  put(&fs.api, "/user/demo/.kv", "save", 4);
  h2_lua_apps_config_t config = {.allocator = mem,
                                 .fs = &fs.api,
                                 .sync = sync,
                                 .preinstalled_index =
                                     "/preinstalled/index.json",
                                 .install_root = "/apps"};
  h2_lua_apps_t *apps = NULL;
  assert(h2_lua_apps_create(&config, &apps) == 0 &&
         h2_lua_apps_can_install(apps));
  check_version(apps, "0.1.0");
  h2_lua_app_info_t info;
  http_t h = {
      .archive = fixture_valid, .size = sizeof(fixture_valid), .chunk = 1};
  h2_pal_http_api_t http = {.user = &h, .vtable = &http_vtable};
  assert(h2_lua_apps_install(apps, &http, URI("https://example.test/app"),
                             fixture_valid_sha256, 1000, cancelled, &h,
                             &info) == 0);
  assert(h.chunks > 100 && !strcmp(info.version, "1.0.0"));
  check_version(apps, "1.0.0");
  expect_file(&fs.api, info.entry, source, sizeof(source) - 1);
  expect_file(&fs.api, "/preinstalled/demo.lua", source, sizeof(source) - 1);
  expect_file(&fs.api, "/user/demo/.kv", "save", 4);
  assert(h2_lua_apps_destroy(apps) == 0);
  assert(h2_lua_apps_create(&config, &apps) == 0);
  check_version(apps, "1.0.0");
  struct bad {
    const unsigned char *bytes;
    size_t size;
  } bad[] = {
#define BAD(name) {fixture_##name, sizeof(fixture_##name)}
      BAD(hash),          BAD(extra),         BAD(duplicate),
      BAD(magic),         BAD(truncated_tar), BAD(unaligned_tar),
      BAD(trailing_tar),  BAD(traversal),     BAD(nul),
      BAD(bytecode),      BAD(large),         BAD(truncated_zlib),
      BAD(trailing_zlib),
#undef BAD
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    h = (http_t){.archive = bad[i].bytes, .size = bad[i].size, .chunk = 7};
    int rc = h2_lua_apps_install(apps, &http, URI("https://example.test/app"),
                                 NULL, 1000, cancelled, &h, &info);
    if (rc == 0)
      fprintf(stderr, "accepted bad archive %zu\n", i);
    assert(rc != 0 && info.app_id[0] == 0);
    check_version(apps, "1.0.0");
    expect_file(&fs.api, "/user/demo/.kv", "save", 4);
  }
  h = (http_t){.archive = fixture_update,
               .size = sizeof(fixture_update),
               .chunk = 7,
               .cancel_after = 2};
  assert(h2_lua_apps_install(apps, &http, URI("https://example.test/app"), NULL,
                             1000, cancelled, &h, &info) == H2_PAL_ERR_CLOSED);
  check_version(apps, "1.0.0");
  h.cancel_after = 0;
  fs.rename =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1, .skip = 2};
  assert(h2_lua_apps_install(apps, &http, URI("https://example.test/app"), NULL,
                             1000, cancelled, &h, &info) != 0);
  check_version(apps, "1.0.0");
  fs.rename = (h2_app_test_fault_t){0};
  h = (http_t){
      .archive = fixture_update, .size = sizeof(fixture_update), .chunk = 511};
  assert(h2_lua_apps_install(apps, &http, URI("https://example.test/app"), NULL,
                             1000, cancelled, &h, &info) == 0);
  check_version(apps, "2.0.0");
  h = (http_t){
      .archive = fixture_data, .size = sizeof(fixture_data), .chunk = 4096};
  assert(h2_lua_apps_install(apps, &http, URI("https://example.test/app"), NULL,
                             1000, cancelled, &h, &info) == 0);
  char resource[H2_LUA_APP_PATH_MAX];
  assert(snprintf(resource, sizeof(resource), "%s/nested/readme.txt",
                  info.data_dir) > 0);
  expect_file(&fs.api, resource, "immutable resource", 18);
  h = (http_t){
      .archive = fixture_unicode, .size = sizeof(fixture_unicode), .chunk = 17};
  assert(h2_lua_apps_install(apps, &http, URI("https://example.test/app"), NULL,
                             1000, NULL, NULL, &info) == 0);
  assert(snprintf(resource, sizeof(resource), "%s/资源.txt", info.data_dir) >
         0);
  expect_file(&fs.api, resource, "immutable resource", 18);
  const size_t downloaded_chunks = h.chunks;
  /* Direct archives use Binary streams; even valid Base64 archives are not
   * download URLs and must be rejected before touching the HTTP provider. */
  assert(h2_lua_apps_install(apps, &http, URI(fixture_data_zlib),
                             fixture_valid_sha256, 1000, NULL, NULL,
                             &info) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_apps_install(apps, &http, URI(fixture_data_octet), NULL, 1000,
                             NULL, NULL, &info) == H2_PAL_ERR_INVALID_ARG);
  assert(h.chunks == downloaded_chunks && info.app_id[0] == 0);
  expect_file(&fs.api, resource, "immutable resource", 18);
  assert(h2_lua_apps_install(
             apps, NULL, URI("data:application/zlib;base64,AB=="), NULL, 1000,
             NULL, NULL, &info) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_apps_install(
             apps, NULL, URI("data:application/zlib;base64,AA=A"), NULL, 1000,
             NULL, NULL, &info) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_apps_install(apps, NULL, URI("https://example.test/app"), NULL,
                             1000, NULL, NULL,
                             &info) == H2_PAL_ERR_UNSUPPORTED);
  h = (http_t){
      .archive = fixture_valid, .size = sizeof(fixture_valid), .chunk = 31};
  assert(h2_lua_apps_install(apps, &http, URI("http://example.test/app"), NULL,
                             1000, NULL, NULL, &info) == 0);
  h.location = "../archive?signature=ok";
  assert(h2_lua_apps_install(apps, &http, URI("http://example.test/app"), NULL,
                             1000, NULL, NULL, &info) == 0);
  h.location = "//user:password@example.test/archive";
  assert(h2_lua_apps_install(apps, &http, URI("https://example.test/app"), NULL,
                             1000, NULL, NULL, &info) != 0);
  h.location = "file:///private/archive";
  assert(h2_lua_apps_install(apps, &http, URI("https://example.test/app"), NULL,
                             1000, NULL, NULL, &info) != 0);
  h.location = NULL;
  h2_lua_app_install_t *install = NULL;
  assert(h2_lua_app_install_begin(apps, sizeof(fixture_valid),
                                  fixture_valid_sha256, NULL, NULL,
                                  &install) == 0);
  for (size_t i = 0; i < sizeof(fixture_valid); ++i)
    assert(h2_lua_app_install_write(install, fixture_valid + i, 1) == 0);
  assert(h2_lua_app_install_finish(&install, &info) == 0 && install == NULL);
  assert(h2_lua_app_install_begin(apps, sizeof(fixture_valid) + 1u, NULL, NULL,
                                  NULL, &install) == 0);
  assert(h2_lua_app_install_write(install, fixture_valid,
                                  sizeof(fixture_valid)) == 0);
  assert(h2_lua_app_install_finish(&install, &info) == H2_PAL_ERR_FORMAT &&
         install == NULL);
  assert(h2_lua_app_install_begin(apps, sizeof(fixture_valid) - 1u, NULL, NULL,
                                  NULL, &install) == 0);
  assert(h2_lua_app_install_write(install, fixture_valid,
                                  sizeof(fixture_valid)) == H2_PAL_ERR_FORMAT);
  assert(h2_lua_app_install_finish(&install, &info) == H2_PAL_ERR_FORMAT &&
         install == NULL);
  assert(h2_lua_app_install_begin(
             apps, sizeof(fixture_valid),
             "0000000000000000000000000000000000000000000000000000000000000000",
             NULL, NULL, &install) == 0);
  assert(h2_lua_app_install_write(install, fixture_valid,
                                  sizeof(fixture_valid)) == 0);
  assert(h2_lua_app_install_finish(&install, &info) == H2_PAL_ERR_FORMAT &&
         install == NULL);
  assert(h2_lua_app_install_begin(apps, sizeof(fixture_valid), NULL, NULL, NULL,
                                  &install) == 0);
  assert(h2_lua_app_install_write(install, fixture_valid, 3) == 0);
  h2_lua_app_install_abort(&install);
  h2_lua_app_install_abort(&install);
  assert(install == NULL);
  check_version(apps, "1.0.0");
  assert(sizeof(fixture_stream) > 128u * 1024u);
  assert(h2_lua_app_install_begin(apps, sizeof(fixture_stream),
                                  fixture_stream_sha256, NULL, NULL,
                                  &install) == 0);
  for (size_t offset = 0; offset < sizeof(fixture_stream);) {
    size_t take = sizeof(fixture_stream) - offset;
    if (take > 32768u)
      take = 32768u;
    assert(h2_lua_app_install_write(install, fixture_stream + offset, take) ==
           0);
    offset += take;
  }
  assert(h2_lua_app_install_finish(&install, &info) == 0 && install == NULL);
  h = (http_t){
      .archive = fixture_valid, .size = sizeof(fixture_valid), .chunk = 31};
  assert(h2_lua_apps_install(apps, &http, URI("http://example.test/app"), NULL,
                             1000, NULL, NULL, &info) == 0);
  assert(h2_lua_apps_destroy(apps) == 0);
  config.installed_limit_bytes = 1;
  assert(h2_lua_apps_create(&config, &apps) == 0);
  h = (http_t){
      .archive = fixture_update, .size = sizeof(fixture_update), .chunk = 7};
  assert(h2_lua_apps_install(apps, &http, URI("https://example.test/app"), NULL,
                             1000, cancelled, &h,
                             &info) == H2_PAL_ERR_NO_SPACE);
  check_version(apps, "1.0.0");
  assert(h2_lua_apps_destroy(apps) == 0);
  assert(fs.active_handles == 0 && h2_app_test_fs_deinit(&fs) == 0);
  puts("Lua app streaming, preinstalled overlay, rollback and persistence "
       "passed");
  return 0;
}
