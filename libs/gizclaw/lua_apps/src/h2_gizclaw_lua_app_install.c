#include "h2_bundle_tar.h"
#include "h2_gizclaw_lua_app_catalog_internal.h"
#include "zlib.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct received_file {
  app_file_t file;
  bool invalid_text;
} received_file_t;

typedef struct h2_gizclaw_lua_app_install {
  h2_gizclaw_lua_app_catalog_t *apps;
  h2_pal_http_cancel_fn cancel;
  void *cancel_user;
  z_stream z;
  bool z_started, z_finished;
  h2_sha256_t compressed_sha, file_sha;
  uint64_t received, expanded, remaining, padding;
  uint64_t limit;
  uint64_t expected_size;
  char expected_sha256[65];
  int error;
  uint8_t header[512];
  size_t header_size, zero_blocks, count;
  bool in_file, is_manifest, manifest_seen;
  char manifest[H2_GIZCLAW_LUA_APP_MANIFEST_MAX];
  size_t manifest_size;
  received_file_t files[H2_GIZCLAW_LUA_APP_FILES_MAX];
  h2_pal_fs_file_t *file;
  uint8_t input[4096], output[4096];
  app_manifest_t parsed;
} install_t;

static bool cancelled(install_t *s) {
  return s->cancel && s->cancel(s->cancel_user);
}
static voidpf z_alloc(voidpf user, uInt items, uInt size) {
  h2_gizclaw_lua_app_catalog_t *apps = user;
  if (size && items > SIZE_MAX / size)
    return NULL;
  return h2_pal_mem_alloc(apps->config.allocator, (size_t)items * size);
}
static void z_free(voidpf user, voidpf value) {
  h2_pal_mem_free(((h2_gizclaw_lua_app_catalog_t *)user)->config.allocator,
                  value);
}
static int stage_path(install_t *s, size_t number,
                      char out[H2_GIZCLAW_LUA_APP_PATH_MAX]) {
  char name[32];
  (void)snprintf(name, sizeof(name), ".stage/%zu", number);
  return h2_gizclaw_lua_app_catalog_join(out, s->apps->root, name);
}
static void clean_stage(install_t *s) {
  for (size_t i = 0; i < H2_GIZCLAW_LUA_APP_FILES_MAX; ++i) {
    char path[H2_GIZCLAW_LUA_APP_PATH_MAX];
    if (stage_path(s, i, path) == H2_PAL_OK)
      (void)h2_pal_fs_remove(s->apps->config.fs, path);
  }
}

static int finish_file(install_t *s) {
  int rc = H2_PAL_OK;
  if (!s->is_manifest) {
    rc = h2_pal_fs_sync(s->apps->config.fs, s->file);
    int close_rc = h2_gizclaw_lua_app_catalog_close(s->apps, s->file);
    s->file = NULL;
    if (rc == H2_PAL_OK)
      rc = close_rc;
    uint8_t digest[32];
    h2_sha256_finish(&s->file_sha, digest);
    h2_sha256_hex(digest, s->files[s->count - 1u].file.sha256);
  }
  s->in_file = false;
  return rc;
}

static int begin_file(install_t *s) {
  h2_bundle_entry_t entry = {0};
  int result = 0;
  int rc = h2_bundle_tar_parse_header(s->header, &entry, &result);
  if (rc != H2_PAL_OK)
    return H2_PAL_ERR_FORMAT;
  if (result == H2_BUNDLE_TAR_HEADER_ZERO) {
    ++s->zero_blocks;
    return H2_PAL_OK;
  }
  if (s->zero_blocks || entry.kind != H2_BUNDLE_ENTRY_FILE ||
      memcmp(s->header + 257u,
             "ustar\0"
             "00",
             8u) ||
      !h2_gizclaw_lua_app_catalog_path_valid(entry.path))
    return H2_PAL_ERR_FORMAT;
  s->remaining = entry.size;
  s->padding = h2_bundle_tar_padding(entry.size);
  s->is_manifest = !strcmp(entry.path, "manifest.json");
  s->in_file = true;
  if (s->is_manifest) {
    if (s->manifest_seen || entry.size == 0 || entry.size > sizeof(s->manifest))
      return H2_PAL_ERR_FORMAT;
    s->manifest_seen = true;
    s->manifest_size = 0;
  } else {
    size_t name_size = strlen(entry.path);
    if (!strchr(entry.path, '/') && name_size >= 4u &&
        !strcmp(entry.path + name_size - 4u, ".lua") &&
        entry.size > s->apps->config.source_limit_bytes)
      return H2_PAL_ERR_NO_SPACE;
    if (s->count == s->apps->config.max_files ||
        entry.size > s->apps->config.app_limit_bytes)
      return H2_PAL_ERR_NO_SPACE;
    for (size_t i = 0; i < s->count; ++i)
      if (!strcmp(s->files[i].file.path, entry.path))
        return H2_PAL_ERR_FORMAT;
    received_file_t *f = &s->files[s->count];
    strcpy(f->file.path, entry.path);
    f->file.size = entry.size;
    char path[H2_GIZCLAW_LUA_APP_PATH_MAX];
    rc = stage_path(s, s->count++, path);
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_lua_app_catalog_mkdirs(s->apps, path);
    if (rc == H2_PAL_OK)
      rc = h2_pal_fs_open(s->apps->config.fs, path,
                          H2_PAL_FS_OPEN_WRITE_TRUNCATE, &s->file);
    if (rc == H2_PAL_OK && !s->file)
      rc = H2_PAL_ERR_IO;
    h2_sha256_init(&s->file_sha);
  }
  if (rc == H2_PAL_OK && s->remaining == 0)
    rc = finish_file(s);
  return rc;
}

static int tar_bytes(install_t *s, const uint8_t *bytes, size_t size) {
  if (size > s->limit - s->expanded)
    return H2_PAL_ERR_NO_SPACE;
  s->expanded += size;
  while (size) {
    if (cancelled(s))
      return H2_PAL_ERR_CLOSED;
    size_t take = size;
    if (s->in_file) {
      if (take > s->remaining)
        take = (size_t)s->remaining;
      if (s->is_manifest) {
        memcpy(s->manifest + s->manifest_size, bytes, take);
        s->manifest_size += take;
      } else {
        received_file_t *f = &s->files[s->count - 1u];
        if (memchr(bytes, 0, take) ||
            (s->remaining == f->file.size && take && bytes[0] == 0x1b))
          f->invalid_text = true;
        h2_sha256_update(&s->file_sha, bytes, take);
        size_t offset = 0;
        while (offset < take) {
          size_t written = 0;
          int rc = h2_pal_fs_write(s->apps->config.fs, s->file, bytes + offset,
                                   take - offset, &written);
          if (rc != H2_PAL_OK)
            return rc;
          if (!written || written > take - offset)
            return H2_PAL_ERR_IO;
          offset += written;
        }
      }
      s->remaining -= take;
      if (!s->remaining) {
        int rc = finish_file(s);
        if (rc != H2_PAL_OK)
          return rc;
      }
    } else if (s->padding) {
      if (take > s->padding)
        take = (size_t)s->padding;
      for (size_t i = 0; i < take; ++i)
        if (bytes[i])
          return H2_PAL_ERR_FORMAT;
      s->padding -= take;
    } else {
      if (take > sizeof(s->header) - s->header_size)
        take = sizeof(s->header) - s->header_size;
      memcpy(s->header + s->header_size, bytes, take);
      s->header_size += take;
      if (s->header_size == sizeof(s->header)) {
        int rc = begin_file(s);
        if (rc != H2_PAL_OK)
          return rc;
        s->header_size = 0;
      }
    }
    bytes += take;
    size -= take;
  }
  return H2_PAL_OK;
}

static int receive(void *user, const h2_pal_http_request_t *request,
                   const uint8_t *bytes, size_t size, size_t total,
                   size_t remaining) {
  (void)request;
  (void)remaining;
  install_t *s = user;
  if (cancelled(s))
    return H2_PAL_ERR_CLOSED;
  if (!size)
    return total == s->received ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
  if (size > UINT_MAX || s->z_finished || size > s->limit - s->received ||
      total != s->received + size)
    return H2_PAL_ERR_FORMAT;
  s->received += size;
  h2_sha256_update(&s->compressed_sha, bytes, size);
  s->z.next_in = (Bytef *)bytes;
  s->z.avail_in = (uInt)size;
  do {
    s->z.next_out = s->output;
    s->z.avail_out = sizeof(s->output);
    uInt before = s->z.avail_in;
    int zrc = inflate(&s->z, Z_NO_FLUSH);
    if (zrc != Z_OK && zrc != Z_STREAM_END && zrc != Z_BUF_ERROR)
      return H2_PAL_ERR_FORMAT;
    size_t produced = sizeof(s->output) - s->z.avail_out;
    int rc = tar_bytes(s, s->output, produced);
    if (rc != H2_PAL_OK)
      return rc;
    if (zrc == Z_STREAM_END) {
      s->z_finished = true;
      return s->z.avail_in ? H2_PAL_ERR_FORMAT : H2_PAL_OK;
    }
    if (!produced && before == s->z.avail_in)
      break;
  } while (s->z.avail_in || s->z.avail_out == 0);
  return s->z.avail_in ? H2_PAL_ERR_FORMAT : H2_PAL_OK;
}

static bool url_valid(h2_gizclaw_lua_app_source_t uri) {
  size_t scheme = uri.size >= 8 && !memcmp(uri.data, "https://", 8)  ? 8u
                  : uri.size >= 7 && !memcmp(uri.data, "http://", 7) ? 7u
                                                                     : 0u;
  if (!scheme || uri.size <= scheme || uri.size > H2_GIZCLAW_LUA_APP_URL_MAX)
    return false;
  size_t host_end = scheme;
  while (host_end < uri.size && uri.data[host_end] != '/' &&
         uri.data[host_end] != '?' && uri.data[host_end] != '#')
    ++host_end;
  if (host_end == scheme)
    return false;
  for (size_t i = 0; i < uri.size; ++i) {
    unsigned char c = (unsigned char)uri.data[i];
    if (c <= 32u || c == 127u || c == '\\' || c == '#' ||
        (i < host_end && c == '@'))
      return false;
  }
  return true;
}

static bool name_is(h2_pal_http_str_t name, const char *expected) {
  if (name.len != strlen(expected))
    return false;
  for (size_t i = 0; i < name.len; ++i) {
    char c = name.data[i];
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
    if (c != expected[i])
      return false;
  }
  return true;
}
static int header(void *user, const h2_pal_http_request_t *request,
                  h2_pal_http_str_t name, h2_pal_http_str_t value) {
  (void)user;
  (void)request;
  if (name_is(name, "location")) {
    char url[1025];
    if (!value.len || value.len >= sizeof(url) ||
        memchr(value.data, 0, value.len))
      return H2_PAL_ERR_FORMAT;
    memcpy(url, value.data, value.len);
    url[value.len] = 0;
    if (!strncmp(url, "http://", 7u) || !strncmp(url, "https://", 8u))
      return url_valid((h2_gizclaw_lua_app_source_t){url, value.len})
                 ? H2_PAL_OK
                 : H2_PAL_ERR_FORMAT;
    if (value.len > 1u && url[0] == '/' && url[1] == '/') {
      const char *scheme = request && request->url.len >= 8u &&
                                   !memcmp(request->url.data, "https://", 8u)
                               ? "https:"
                               : "http:";
      size_t prefix = strlen(scheme);
      if (value.len + prefix >= sizeof(url))
        return H2_PAL_ERR_FORMAT;
      memmove(url + prefix, url, value.len + 1u);
      memcpy(url, scheme, prefix);
      return url_valid((h2_gizclaw_lua_app_source_t){url, value.len + prefix})
                 ? H2_PAL_OK
                 : H2_PAL_ERR_FORMAT;
    }
    /* Relative redirects are resolved by HTTP PAL against its current URL.
     */
    size_t head = strcspn(url, "/?");
    for (size_t i = 0; i < value.len; ++i) {
      unsigned char c = (unsigned char)url[i];
      if (c <= 32u || c == 127u || c == '\\' || c == '#' ||
          (i < head && c == ':'))
        return H2_PAL_ERR_FORMAT;
    }
  }
  return H2_PAL_OK;
}

static int verify(install_t *s, const char *expected) {
  if (!s->z_finished || s->in_file || s->padding || s->header_size ||
      s->zero_blocks < 2u || !s->manifest_seen)
    return H2_PAL_ERR_FORMAT;
  uint8_t digest[32];
  char hex[65];
  h2_sha256_finish(&s->compressed_sha, digest);
  h2_sha256_hex(digest, hex);
  if (expected && *expected) {
    if (strlen(expected) != 64u)
      return H2_PAL_ERR_INVALID_ARG;
    for (size_t i = 0; i < 64u; ++i) {
      char c = expected[i];
      if (c >= 'A' && c <= 'F')
        c += 'a' - 'A';
      if (c != hex[i])
        return H2_PAL_ERR_FORMAT;
    }
  }
  yyjson_doc *doc = yyjson_read_opts(s->manifest, s->manifest_size, 0,
                                     &s->apps->json_allocator, NULL);
  int rc = doc ? h2_gizclaw_lua_app_catalog_manifest(
                     s->apps, yyjson_doc_get_root(doc), &s->parsed)
               : H2_PAL_ERR_FORMAT;
  yyjson_doc_free(doc);
  if (rc != H2_PAL_OK)
    return rc;
  if (s->count != s->parsed.count)
    return H2_PAL_ERR_FORMAT;
  for (size_t i = 0; i < s->parsed.count; ++i) {
    app_file_t *f = &s->parsed.files[i];
    bool found = false;
    for (size_t j = 0; j < s->count; ++j) {
      received_file_t *r = &s->files[j];
      if (strcmp(f->path, r->file.path))
        continue;
      if (f->size != r->file.size || strcmp(f->sha256, r->file.sha256) ||
          (!strcmp(f->path, s->parsed.entry) && r->invalid_text))
        return H2_PAL_ERR_FORMAT;
      found = true;
    }
    if (!found)
      return H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}

static int commit(install_t *s, h2_gizclaw_lua_app_info_t *out) {
  h2_gizclaw_lua_app_catalog_t *apps = s->apps;
  int position = h2_gizclaw_lua_app_catalog_find(apps, s->parsed.info.app_id);
  if (h2_gizclaw_lua_app_catalog_pinned(apps, s->parsed.info.app_id))
    return H2_PAL_ERR_BUSY;
  if (position < 0 && apps->count == apps->config.max_apps)
    return H2_PAL_ERR_NO_SPACE;
  uint64_t used = 0;
  for (size_t i = 0; i < apps->count; ++i) {
    if ((int)i == position || apps->records[i].slot < 0)
      continue;
    if (apps->records[i].bytes > apps->config.installed_limit_bytes - used)
      return H2_PAL_ERR_NO_SPACE;
    used += apps->records[i].bytes;
  }
  if (s->parsed.bytes > apps->config.installed_limit_bytes - used)
    return H2_PAL_ERR_NO_SPACE;
  int old_slot = position < 0 ? -1 : apps->records[position].slot;
  int slot = old_slot == 0 ? 1 : 0;
  /* Directory names themselves may alias on the backend. Check both slots
   * before any cleanup so a new identity cannot erase another app's files. */
  for (unsigned candidate = 0; candidate < 2u; ++candidate) {
    char relative[40], location[H2_GIZCLAW_LUA_APP_PATH_MAX];
    (void)snprintf(relative, sizeof(relative), "%s/%u", s->parsed.info.app_id,
                   candidate);
    int rc = h2_gizclaw_lua_app_catalog_join(location, apps->root, relative);
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_lua_app_catalog_check_slot(apps, location,
                                                 s->parsed.info.app_id);
    if (rc != H2_PAL_OK)
      return rc;
  }
  char name[40], base[H2_GIZCLAW_LUA_APP_PATH_MAX] = {0},
                 manifest_path[H2_GIZCLAW_LUA_APP_PATH_MAX],
                 manifest_tmp[H2_GIZCLAW_LUA_APP_PATH_MAX];
  (void)snprintf(name, sizeof(name), "%s/%c", s->parsed.info.app_id,
                 slot == 1 ? '1' : '0');
  int rc = h2_gizclaw_lua_app_catalog_join(base, apps->root, name);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_lua_app_catalog_remove_slot(apps, base,
                                                s->parsed.info.app_id);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_lua_app_catalog_join(manifest_path, base, "manifest.json");
  /* Persist the cleanup inventory before moving any staged payload. A power
   * loss can therefore be recovered without a filesystem enumeration API. */
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_lua_app_catalog_join(manifest_tmp, base, "manifest.tmp");
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_lua_app_catalog_write(apps, manifest_tmp, s->manifest,
                                          s->manifest_size);
  if (rc == H2_PAL_OK)
    rc = h2_pal_fs_rename(apps->config.fs, manifest_tmp, manifest_path);
  for (size_t i = 0; rc == H2_PAL_OK && i < s->count; ++i) {
    if (cancelled(s)) {
      rc = H2_PAL_ERR_CLOSED;
      break;
    }
    char from[H2_GIZCLAW_LUA_APP_PATH_MAX], to[H2_GIZCLAW_LUA_APP_PATH_MAX];
    rc = stage_path(s, i, from);
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_lua_app_catalog_join(to, base, s->files[i].file.path);
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_lua_app_catalog_mkdirs(apps, to);
    if (rc == H2_PAL_OK) {
      h2_pal_fs_stat_t existing = {0};
      int stat_rc = h2_pal_fs_stat(apps->config.fs, to, &existing);
      /* Filesystems may normalize case, Unicode or trailing dots. Never
       * overwrite a different payload path that aliases an earlier
       * member. */
      rc = stat_rc == H2_PAL_ERR_NOT_FOUND ? H2_PAL_OK
           : stat_rc == H2_PAL_OK          ? H2_PAL_ERR_FORMAT
                                           : stat_rc;
    }
    if (rc == H2_PAL_OK)
      rc = h2_pal_fs_rename(apps->config.fs, from, to);
  }
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_lua_app_catalog_resolve(apps, &s->parsed, base);
  if (rc == H2_PAL_OK && cancelled(s))
    rc = H2_PAL_ERR_CLOSED;
  if (rc == H2_PAL_OK) {
    app_record_t record = {
        .info = s->parsed.info,
        .bytes = s->parsed.bytes,
        .slot = slot,
        .preinstalled = position >= 0 && apps->records[position].preinstalled};
    rc = h2_gizclaw_lua_app_catalog_publish(
        apps, &record, position < 0 ? apps->count : (size_t)position);
  }
  if (rc != H2_PAL_OK) {
    if (*base)
      (void)h2_gizclaw_lua_app_catalog_remove_slot(apps, base,
                                                   s->parsed.info.app_id);
    return rc;
  }
  *out = s->parsed.info;
  /* Once publication succeeds cleanup cannot turn the committed installation
   * into a reported failure. The inactive slot is retried before its next
   * use.
   */
  if (old_slot >= 0) {
    (void)snprintf(name, sizeof(name), "%s/%c", s->parsed.info.app_id,
                   old_slot == 1 ? '1' : '0');
    if (h2_gizclaw_lua_app_catalog_join(base, apps->root, name) == H2_PAL_OK)
      (void)h2_gizclaw_lua_app_catalog_remove_slot(apps, base,
                                                   s->parsed.info.app_id);
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_lua_app_install_begin(
    h2_gizclaw_lua_app_catalog_t *apps, uint64_t compressed_size,
    const char *sha256, h2_pal_http_cancel_fn cancel, void *cancel_user,
    h2_gizclaw_lua_app_install_t **out_install) {
  if (out_install == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_install = NULL;
  if (sha256 && (strlen(sha256) != 64u ||
                 strspn(sha256, "0123456789abcdefABCDEF") != 64u))
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_lua_app_catalog_can_install(apps))
    return H2_PAL_ERR_UNSUPPORTED;
  int rc = h2_gizclaw_lua_app_catalog_try_lock(apps);
  if (rc != H2_PAL_OK)
    return rc;
  if (apps->close_error) {
    rc = apps->close_error;
    h2_gizclaw_lua_app_catalog_unlock(apps);
    return rc;
  }
  install_t *s = h2_pal_mem_alloc(apps->config.allocator, sizeof(*s));
  if (!s) {
    h2_gizclaw_lua_app_catalog_unlock(apps);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(s, 0, sizeof(*s));
  s->apps = apps;
  s->cancel = cancel;
  s->cancel_user = cancel_user;
  s->expected_size = compressed_size;
  if (sha256)
    strcpy(s->expected_sha256, sha256);
  s->limit = apps->config.app_limit_bytes + H2_GIZCLAW_LUA_APP_MANIFEST_MAX +
             (apps->config.max_files + 2u) * 1024u + 16384u;
  if (compressed_size > s->limit) {
    h2_pal_mem_free(apps->config.allocator, s);
    h2_gizclaw_lua_app_catalog_unlock(apps);
    return H2_PAL_ERR_NO_SPACE;
  }
  clean_stage(s);
  h2_sha256_init(&s->compressed_sha);
  s->z.zalloc = z_alloc;
  s->z.zfree = z_free;
  s->z.opaque = apps;
  rc = inflateInit(&s->z) == Z_OK ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
  s->z_started = rc == H2_PAL_OK;
  if (rc == H2_PAL_OK && cancelled(s))
    rc = H2_PAL_ERR_CLOSED;
  if (rc != H2_PAL_OK) {
    h2_gizclaw_lua_app_install_abort(&s);
    return rc;
  }
  *out_install = s;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_lua_app_install_write(h2_gizclaw_lua_app_install_t *s,
                                 const uint8_t *bytes, size_t size) {
  if (!s || (!bytes && size))
    return H2_PAL_ERR_INVALID_ARG;
  if (s->error != H2_PAL_OK)
    return s->error;
  if (s->expected_size && size > s->expected_size - s->received)
    return s->error = H2_PAL_ERR_FORMAT;
  if (!size)
    return H2_PAL_OK;
  return s->error =
             receive(s, NULL, bytes, size, (size_t)s->received + size, 0);
}

void h2_gizclaw_lua_app_install_abort(h2_gizclaw_lua_app_install_t **install) {
  if (!install || !*install)
    return;
  install_t *s = *install;
  *install = NULL;
  h2_gizclaw_lua_app_catalog_t *apps = s->apps;
  if (s->file)
    (void)h2_gizclaw_lua_app_catalog_close(apps, s->file);
  if (s->z_started)
    (void)inflateEnd(&s->z);
  clean_stage(s);
  h2_pal_mem_free(apps->config.allocator, s);
  h2_gizclaw_lua_app_catalog_unlock(apps);
}

h2_pal_result_t
h2_gizclaw_lua_app_install_finish(h2_gizclaw_lua_app_install_t **install,
                                  h2_gizclaw_lua_app_info_t *out) {
  if (!install || !*install)
    return H2_PAL_ERR_INVALID_ARG;
  if (out)
    memset(out, 0, sizeof(*out));
  install_t *s = *install;
  int rc = !out ? H2_PAL_ERR_INVALID_ARG : s->error;
  if (rc == H2_PAL_OK && s->expected_size && s->expected_size != s->received)
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK && cancelled(s))
    rc = H2_PAL_ERR_CLOSED;
  if (rc == H2_PAL_OK)
    rc = verify(s, s->expected_sha256);
  if (rc == H2_PAL_OK)
    rc = commit(s, out);
  h2_gizclaw_lua_app_install_abort(install);
  return rc;
}

h2_pal_result_t h2_gizclaw_lua_app_catalog_install(
    h2_gizclaw_lua_app_catalog_t *apps, const h2_pal_http_api_t *http,
    h2_gizclaw_lua_app_source_t source, const char *sha256, uint32_t timeout_ms,
    h2_pal_http_cancel_fn cancel, void *cancel_user,
    h2_gizclaw_lua_app_info_t *out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (!source.data || !url_valid(source) || !timeout_ms ||
      timeout_ms > 110000u || (sha256 && *sha256 && strlen(sha256) != 64u))
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_lua_app_catalog_can_install(apps) || http == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  install_t *s = NULL;
  int rc = h2_gizclaw_lua_app_install_begin(
      apps, 0, sha256 && *sha256 ? sha256 : NULL, cancel, cancel_user, &s);
  if (rc != H2_PAL_OK)
    return rc;
  h2_pal_http_request_t request = {.method = H2_PAL_HTTP_GET,
                                   .url = {source.data, source.size},
                                   .timeout_ms = (int)timeout_ms,
                                   .retry_count = 0,
                                   .chunk_buf = s->input,
                                   .chunk_buf_cap = sizeof(s->input),
                                   .read_cb = receive,
                                   .user = s,
                                   .cancel_cb = cancel,
                                   .cancel_user = cancel_user,
                                   .response_header_cb = header,
                                   .allocator = apps->config.allocator};
  h2_pal_http_response_t response = {0};
  if (rc == H2_PAL_OK && cancelled(s))
    rc = H2_PAL_ERR_CLOSED;
  if (rc == H2_PAL_OK)
    rc = h2_pal_http_request(http, &request, &response);
  if (rc == H2_PAL_OK && (response.status_code != 200 ||
                          (response.content_length >= 0 &&
                           (uint64_t)response.content_length != s->received)))
    rc = H2_PAL_ERR_IO;
  h2_pal_http_response_free(http, &response);
  if (rc == H2_PAL_OK)
    return h2_gizclaw_lua_app_install_finish(&s, out);
  h2_gizclaw_lua_app_install_abort(&s);
  return rc;
}
