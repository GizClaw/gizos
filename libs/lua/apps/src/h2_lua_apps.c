#include "h2_lua_apps_internal.h"
#include "h2_semver.h"

#include <stdio.h>
#include <string.h>

static void *json_malloc(void *ctx, size_t size) {
  return h2_pal_mem_alloc(ctx, size);
}
static void *json_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
  void *next = h2_pal_mem_alloc(ctx, size);
  if (next != NULL) {
    if (ptr != NULL)
      memcpy(next, ptr, old_size < size ? old_size : size);
    h2_pal_mem_free(ctx, ptr);
  }
  return next;
}
static void json_free(void *ctx, void *ptr) { h2_pal_mem_free(ctx, ptr); }

bool h2_lua_app_id_valid(const char *id) {
  if (id == NULL || id[0] == '\0' || id[0] == '.' || strlen(id) > 32u)
    return false;
  for (; *id; ++id)
    if (!(*id >= 'a' && *id <= 'z') && !(*id >= '0' && *id <= '9') &&
        *id != '_' && *id != '-' && *id != '.')
      return false;
  return true;
}

bool h2_lua_apps_path_valid(const char *path) {
  if (path == NULL || !*path || *path == '/' ||
      strlen(path) >= H2_LUA_APP_PATH_MAX)
    return false;
  bool start = true;
  for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
    if (*p < 32u || *p == 127u || *p == '\\' || *p == ':' ||
        (start && (*p == '.' || *p == '/')))
      return false;
    start = *p == '/';
  }
  return !start;
}

static bool root_valid(const char *root) {
  return root != NULL && *root && strlen(root) < H2_LUA_APP_PATH_MAX &&
         h2_lua_apps_path_valid(root[0] == '/' ? root + 1 : root);
}

int h2_lua_apps_join(char out[H2_LUA_APP_PATH_MAX], const char *root,
                     const char *name) {
  int n = snprintf(out, H2_LUA_APP_PATH_MAX, "%s/%s", root, name);
  return n >= 0 && (size_t)n < H2_LUA_APP_PATH_MAX ? H2_PAL_OK
                                                   : H2_PAL_ERR_NO_SPACE;
}

int h2_lua_apps_try_lock(h2_lua_apps_t *apps) {
  if (apps == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_pal_mutex_try_lock(apps->config.sync, apps->mutex);
  return rc == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_ERR_BUSY : rc;
}
void h2_lua_apps_unlock(h2_lua_apps_t *apps) {
  (void)h2_pal_mutex_unlock(apps->config.sync, apps->mutex);
}
int h2_lua_apps_find(const h2_lua_apps_t *apps, const char *id) {
  for (size_t i = 0; i < apps->count; ++i)
    if (!strcmp(apps->records[i].info.app_id, id))
      return (int)i;
  return -1;
}
int h2_lua_apps_close(h2_lua_apps_t *apps, h2_pal_fs_file_t *file) {
  int rc = h2_pal_fs_close(apps->config.fs, file);
  if (rc != H2_PAL_OK && !apps->close_error) {
    apps->close_error = rc;
    apps->unclosed_file = file;
  }
  return rc;
}

int h2_lua_apps_read(h2_lua_apps_t *apps, const char *path, size_t limit,
                     uint8_t **out, size_t *out_size) {
  *out = NULL;
  *out_size = 0;
  if (apps->close_error)
    return apps->close_error;
  h2_pal_fs_stat_t stat = {0};
  int rc = h2_pal_fs_stat(apps->config.fs, path, &stat);
  if (rc != H2_PAL_OK)
    return rc;
  if (stat.is_dir || stat.size > limit)
    return H2_PAL_ERR_FORMAT;
  uint8_t *bytes =
      h2_pal_mem_alloc(apps->config.allocator, (size_t)stat.size + 1u);
  if (bytes == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  h2_pal_fs_file_t *file = NULL;
  rc = h2_pal_fs_open(apps->config.fs, path, H2_PAL_FS_OPEN_READ, &file);
  size_t offset = 0;
  if (rc == H2_PAL_OK && file == NULL)
    rc = H2_PAL_ERR_IO;
  while (rc == H2_PAL_OK && offset < stat.size) {
    size_t count = 0;
    rc = h2_pal_fs_read(apps->config.fs, file, bytes + offset,
                        (size_t)stat.size - offset, &count);
    if (rc == H2_PAL_OK && (count == 0 || count > stat.size - offset))
      rc = H2_PAL_ERR_IO;
    if (rc == H2_PAL_OK)
      offset += count;
  }
  if (file != NULL) {
    int close_rc = h2_lua_apps_close(apps, file);
    if (rc == H2_PAL_OK)
      rc = close_rc;
  }
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(apps->config.allocator, bytes);
    return rc;
  }
  bytes[offset] = 0;
  *out = bytes;
  *out_size = offset;
  return H2_PAL_OK;
}

int h2_lua_apps_mkdirs(h2_lua_apps_t *apps, const char *file_path) {
  if (strlen(file_path) >= H2_LUA_APP_PATH_MAX)
    return H2_PAL_ERR_NO_SPACE;
  char path[H2_LUA_APP_PATH_MAX];
  strcpy(path, file_path);
  for (char *p = path + 1; *p; ++p) {
    if (*p != '/')
      continue;
    *p = 0;
    h2_pal_fs_stat_t stat = {0};
    int rc = h2_pal_fs_stat(apps->config.fs, path, &stat);
    if (rc == H2_PAL_ERR_NOT_FOUND)
      rc = h2_pal_fs_mkdir(apps->config.fs, path);
    else if (rc == H2_PAL_OK && !stat.is_dir)
      rc = H2_PAL_ERR_FORMAT;
    *p = '/';
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}

int h2_lua_apps_write(h2_lua_apps_t *apps, const char *path, const void *data,
                      size_t size) {
  if (apps->close_error)
    return apps->close_error;
  int rc = h2_lua_apps_mkdirs(apps, path);
  h2_pal_fs_file_t *file = NULL;
  if (rc == H2_PAL_OK)
    rc = h2_pal_fs_open(apps->config.fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE,
                        &file);
  if (rc == H2_PAL_OK && file == NULL)
    rc = H2_PAL_ERR_IO;
  const uint8_t *bytes = data;
  size_t offset = 0;
  while (rc == H2_PAL_OK && offset < size) {
    size_t count = 0;
    rc = h2_pal_fs_write(apps->config.fs, file, bytes + offset, size - offset,
                         &count);
    if (rc == H2_PAL_OK && (!count || count > size - offset))
      rc = H2_PAL_ERR_IO;
    if (rc == H2_PAL_OK)
      offset += count;
  }
  if (rc == H2_PAL_OK)
    rc = h2_pal_fs_sync(apps->config.fs, file);
  if (file != NULL) {
    int close_rc = h2_lua_apps_close(apps, file);
    if (rc == H2_PAL_OK)
      rc = close_rc;
  }
  return rc;
}

static bool text(yyjson_val *v, char *out, size_t capacity, bool optional) {
  if (optional && (v == NULL || yyjson_is_null(v))) {
    out[0] = 0;
    return true;
  }
  if (!yyjson_is_str(v))
    return false;
  size_t len = yyjson_get_len(v);
  if (len >= capacity || memchr(yyjson_get_str(v), 0, len))
    return false;
  memcpy(out, yyjson_get_str(v), len);
  out[len] = 0;
  return true;
}

static bool unique_keys(yyjson_val *value) {
  if (!yyjson_is_obj(value))
    return false;
  size_t i, max;
  yyjson_val *key, *val;
  yyjson_obj_foreach(value, i, max, key, val) {
    (void)val;
    size_t j, count;
    yyjson_val *other, *ignored;
    yyjson_obj_foreach(value, j, count, other, ignored) {
      (void)ignored;
      if (j < i && yyjson_get_len(key) == yyjson_get_len(other) &&
          !memcmp(yyjson_get_str(key), yyjson_get_str(other),
                  yyjson_get_len(key)))
        return false;
    }
  }
  return true;
}

int h2_lua_apps_manifest(h2_lua_apps_t *apps, yyjson_val *v,
                         app_manifest_t *m) {
  memset(m, 0, sizeof(*m));
  if (!unique_keys(v) ||
      !yyjson_equals_str(yyjson_obj_get(v, "type"), "lua-app") ||
      !yyjson_is_uint(yyjson_obj_get(v, "format")) ||
      yyjson_get_uint(yyjson_obj_get(v, "format")) != 1 ||
      !text(yyjson_obj_get(v, "app_id"), m->info.app_id, sizeof(m->info.app_id),
            false) ||
      !h2_lua_app_id_valid(m->info.app_id) ||
      !text(yyjson_obj_get(v, "version"), m->info.version,
            sizeof(m->info.version), false) ||
      !h2_semver_is_valid(m->info.version) ||
      !text(yyjson_obj_get(v, "entry"), m->entry, sizeof(m->entry), false) ||
      !text(yyjson_obj_get(v, "data_dir"), m->data_dir, sizeof(m->data_dir),
            true) ||
      !text(yyjson_obj_get(v, "display_name"), m->info.display_name,
            sizeof(m->info.display_name), true) ||
      !text(yyjson_obj_get(v, "description"), m->info.description,
            sizeof(m->info.description), true))
    return H2_PAL_ERR_FORMAT;
  char entry[37];
  (void)snprintf(entry, sizeof(entry), "%s.lua", m->info.app_id);
  if (strcmp(entry, m->entry) ||
      (*m->data_dir && strcmp(m->data_dir, m->info.app_id)))
    return H2_PAL_ERR_FORMAT;
  yyjson_val *files = yyjson_obj_get(v, "files");
  m->count = yyjson_arr_size(files);
  if (!yyjson_is_arr(files) || m->count == 0 ||
      m->count > apps->config.max_files)
    return H2_PAL_ERR_NO_SPACE;
  bool found_entry = false, found_data = false;
  for (size_t i = 0; i < m->count; ++i) {
    yyjson_val *f = yyjson_arr_get(files, i);
    app_file_t *file = &m->files[i];
    yyjson_val *size = yyjson_obj_get(f, "size");
    if (!unique_keys(f) ||
        !text(yyjson_obj_get(f, "path"), file->path, sizeof(file->path),
              false) ||
        !h2_lua_apps_path_valid(file->path) || !yyjson_is_uint(size) ||
        !text(yyjson_obj_get(f, "sha256"), file->sha256, sizeof(file->sha256),
              false) ||
        strlen(file->sha256) != 64)
      return H2_PAL_ERR_FORMAT;
    for (size_t h = 0; h < 64; ++h)
      if (!((file->sha256[h] >= '0' && file->sha256[h] <= '9') ||
            (file->sha256[h] >= 'a' && file->sha256[h] <= 'f')))
        return H2_PAL_ERR_FORMAT;
    file->size = yyjson_get_uint(size);
    if (file->size > apps->config.app_limit_bytes - m->bytes)
      return H2_PAL_ERR_NO_SPACE;
    m->bytes += file->size;
    if (!strcmp(file->path, m->entry)) {
      if (found_entry || file->size > apps->config.source_limit_bytes)
        return H2_PAL_ERR_FORMAT;
      found_entry = true;
    } else {
      size_t n = strlen(m->data_dir);
      if (!n || strncmp(file->path, m->data_dir, n) || file->path[n] != '/')
        return H2_PAL_ERR_FORMAT;
      found_data = true;
    }
    for (size_t j = 0; j < i; ++j) {
      const char *a = file->path, *b = m->files[j].path;
      while (*a && *b) {
        char ac = *a, bc = *b;
        if (ac >= 'A' && ac <= 'Z')
          ac += 'a' - 'A';
        if (bc >= 'A' && bc <= 'Z')
          bc += 'a' - 'A';
        if (ac != bc)
          break;
        ++a;
        ++b;
      }
      if ((!*a && (!*b || *b == '/')) || (!*b && *a == '/'))
        return H2_PAL_ERR_FORMAT;
    }
  }
  return found_entry && found_data == (*m->data_dir != 0) ? H2_PAL_OK
                                                          : H2_PAL_ERR_FORMAT;
}

int h2_lua_apps_resolve(h2_lua_apps_t *apps, app_manifest_t *m,
                        const char *root) {
  int rc = h2_lua_apps_join(m->info.entry, root, m->entry);
  if (rc == H2_PAL_OK && *m->data_dir)
    rc = h2_lua_apps_join(m->info.data_dir, root, m->data_dir);
  for (size_t i = 0; rc == H2_PAL_OK && i < m->count; ++i) {
    char path[H2_LUA_APP_PATH_MAX];
    h2_pal_fs_stat_t stat = {0};
    rc = h2_lua_apps_join(path, root, m->files[i].path);
    if (rc == H2_PAL_OK)
      rc = h2_pal_fs_stat(apps->config.fs, path, &stat);
    if (rc == H2_PAL_OK && (stat.is_dir || stat.size != m->files[i].size ||
                            (!strcmp(m->files[i].path, m->entry) &&
                             stat.size > apps->config.source_limit_bytes)))
      rc = H2_PAL_ERR_FORMAT;
  }
  return rc;
}

static int read_manifest(h2_lua_apps_t *apps, const char *root,
                         app_manifest_t *m) {
  char path[H2_LUA_APP_PATH_MAX];
  uint8_t *bytes = NULL;
  size_t len = 0;
  int rc = h2_lua_apps_join(path, root, "manifest.json");
  if (rc == H2_PAL_OK)
    rc = h2_lua_apps_read(apps, path, H2_LUA_APP_MANIFEST_MAX, &bytes, &len);
  yyjson_doc *doc =
      rc == H2_PAL_OK
          ? yyjson_read_opts((char *)bytes, len, 0, &apps->json_allocator, NULL)
          : NULL;
  if (rc == H2_PAL_OK)
    rc = doc ? h2_lua_apps_manifest(apps, yyjson_doc_get_root(doc), m)
             : H2_PAL_ERR_FORMAT;
  yyjson_doc_free(doc);
  h2_pal_mem_free(apps->config.allocator, bytes);
  return rc;
}

static int load_index(h2_lua_apps_t *apps, const char *path, bool installed) {
  uint8_t *bytes = NULL;
  size_t len = 0;
  int rc = h2_lua_apps_read(apps, path, H2_LUA_APPS_INDEX_MAX, &bytes, &len);
  if (rc == H2_PAL_ERR_NOT_FOUND)
    return H2_PAL_OK;
  if (rc != H2_PAL_OK)
    return rc;
  yyjson_doc *doc =
      yyjson_read_opts((char *)bytes, len, 0, &apps->json_allocator, NULL);
  yyjson_val *root = yyjson_doc_get_root(doc),
             *array = yyjson_obj_get(root, "apps");
  if (!doc || !unique_keys(root) ||
      !yyjson_is_uint(yyjson_obj_get(root, "format")) ||
      yyjson_get_uint(yyjson_obj_get(root, "format")) != 1 ||
      !yyjson_is_arr(array) || yyjson_arr_size(array) > apps->config.max_apps)
    rc = H2_PAL_ERR_FORMAT;
  app_manifest_t *manifest =
      h2_pal_mem_alloc(apps->config.allocator, sizeof(*manifest));
  if (manifest == NULL)
    rc = H2_PAL_ERR_NO_MEMORY;
  for (size_t i = 0; rc == H2_PAL_OK && i < yyjson_arr_size(array); ++i) {
    yyjson_val *v = yyjson_arr_get(array, i);
    char base[H2_LUA_APP_PATH_MAX];
    int slot = -1;
    if (!unique_keys(v)) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    if (installed) {
      char id[33], name[40];
      yyjson_val *s = yyjson_obj_get(v, "slot");
      if (!text(yyjson_obj_get(v, "app_id"), id, sizeof(id), false) ||
          !h2_lua_app_id_valid(id) || !yyjson_is_uint(s) ||
          yyjson_get_uint(s) > 1) {
        rc = H2_PAL_ERR_FORMAT;
        break;
      }
      slot = (int)yyjson_get_uint(s);
      (void)snprintf(name, sizeof(name), "%s/%c", id, slot == 1 ? '1' : '0');
      rc = h2_lua_apps_join(base, apps->root, name);
      if (rc == H2_PAL_OK)
        rc = read_manifest(apps, base, manifest);
      if (rc == H2_PAL_OK && strcmp(id, manifest->info.app_id))
        rc = H2_PAL_ERR_FORMAT;
    } else {
      if (!text(yyjson_obj_get(v, "root"), base, sizeof(base), false) ||
          !root_valid(base))
        rc = H2_PAL_ERR_FORMAT;
      else
        rc =
            h2_lua_apps_manifest(apps, yyjson_obj_get(v, "manifest"), manifest);
    }
    if (rc != H2_PAL_OK)
      break;
    rc = h2_lua_apps_resolve(apps, manifest, base);
    if (rc == H2_PAL_ERR_NOT_FOUND || rc == H2_PAL_ERR_FORMAT) {
      rc = H2_PAL_OK;
      continue;
    }
    if (rc != H2_PAL_OK)
      break;
    int position = h2_lua_apps_find(apps, manifest->info.app_id);
    if (position >= 0 && (!installed || apps->records[position].slot >= 0)) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    if (position < 0) {
      if (apps->count == apps->config.max_apps) {
        rc = H2_PAL_ERR_NO_SPACE;
        break;
      }
      position = (int)apps->count++;
    }
    apps->records[position] = (app_record_t){
        .info = manifest->info, .bytes = manifest->bytes, .slot = slot};
  }
  h2_pal_mem_free(apps->config.allocator, manifest);
  yyjson_doc_free(doc);
  h2_pal_mem_free(apps->config.allocator, bytes);
  return rc;
}

h2_pal_result_t h2_lua_apps_create(const h2_lua_apps_config_t *c,
                                   h2_lua_apps_t **out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (c == NULL || c->allocator == NULL || c->fs == NULL ||
      c->fs->vtable == NULL || !c->fs->vtable->open || !c->fs->vtable->read ||
      !c->fs->vtable->close || !c->fs->vtable->stat || c->sync == NULL ||
      c->sync->vtable == NULL || !c->sync->vtable->create_mutex ||
      !c->sync->vtable->try_lock_mutex || !c->sync->vtable->unlock_mutex ||
      !c->sync->vtable->destroy_mutex ||
      (c->install_root && !root_valid(c->install_root)) ||
      (c->preinstalled_index && !root_valid(c->preinstalled_index)) ||
      c->max_apps > H2_LUA_APPS_MAX || c->max_files > H2_LUA_APP_FILES_MAX ||
      c->app_limit_bytes > 64u * 1024u * 1024u ||
      c->installed_limit_bytes > 1024u * 1024u * 1024u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_lua_apps_t *apps = h2_pal_mem_alloc(c->allocator, sizeof(*apps));
  if (apps == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(apps, 0, sizeof(*apps));
  apps->config = *c;
  apps->config.preinstalled_index = NULL;
  apps->config.install_root = NULL;
  apps->config.max_apps = c->max_apps ? c->max_apps : H2_LUA_APPS_MAX;
  apps->config.max_files = c->max_files ? c->max_files : H2_LUA_APP_FILES_MAX;
  apps->config.source_limit_bytes =
      c->source_limit_bytes ? c->source_limit_bytes : 256u * 1024u;
  apps->config.app_limit_bytes =
      c->app_limit_bytes ? c->app_limit_bytes : 2u * 1024u * 1024u;
  apps->config.installed_limit_bytes =
      c->installed_limit_bytes ? c->installed_limit_bytes : 8u * 1024u * 1024u;
  apps->json_allocator =
      (yyjson_alc){json_malloc, json_realloc, json_free, (void *)c->allocator};
  if (c->install_root)
    strcpy(apps->root, c->install_root);
  const h2_pal_mutex_config_t mutex = {.name = "lua-apps",
                                       .allocator = c->allocator};
  int rc = h2_pal_mutex_create(c->sync, &mutex, &apps->mutex);
  if (rc == H2_PAL_OK && c->preinstalled_index)
    rc = load_index(apps, c->preinstalled_index, false);
  if (rc == H2_PAL_OK && *apps->root) {
    char path[H2_LUA_APP_PATH_MAX];
    rc = h2_lua_apps_join(path, apps->root, "index.json");
    if (rc == H2_PAL_OK)
      rc = load_index(apps, path, true);
  }
  if (rc != H2_PAL_OK) {
    if (apps->close_error) {
      *out = apps;
      return rc;
    }
    if (apps->mutex)
      (void)h2_pal_mutex_destroy(c->sync, apps->mutex);
    h2_pal_mem_free(c->allocator, apps);
    return rc;
  }
  apps->ready = true;
  *out = apps;
  return H2_PAL_OK;
}
h2_pal_result_t h2_lua_apps_destroy(h2_lua_apps_t *apps) {
  if (apps == NULL)
    return H2_PAL_OK;
  int rc = h2_lua_apps_try_lock(apps);
  if (rc != H2_PAL_OK)
    return rc;
  if (apps->close_error) {
    rc = apps->close_error;
    h2_lua_apps_unlock(apps);
    return rc;
  }
  h2_lua_apps_unlock(apps);
  rc = h2_pal_mutex_destroy(apps->config.sync, apps->mutex);
  if (rc == H2_PAL_OK)
    h2_pal_mem_free(apps->config.allocator, apps);
  return rc;
}
h2_pal_result_t h2_lua_apps_list(h2_lua_apps_t *apps, h2_lua_app_info_t *out,
                                 size_t capacity, size_t *count) {
  if (count == NULL || (capacity && out == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  *count = 0;
  int rc = h2_lua_apps_try_lock(apps);
  if (rc != H2_PAL_OK)
    return rc;
  if (!apps->ready) {
    h2_lua_apps_unlock(apps);
    return H2_PAL_ERR_INVALID_STATE;
  }
  *count = apps->count;
  if (out && capacity < apps->count)
    rc = H2_PAL_ERR_NO_SPACE;
  else if (out)
    for (size_t i = 0; i < apps->count; ++i)
      out[i] = apps->records[i].info;
  h2_lua_apps_unlock(apps);
  return rc;
}
h2_pal_result_t h2_lua_apps_get(h2_lua_apps_t *apps, const char *id,
                                h2_lua_app_info_t *out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (!h2_lua_app_id_valid(id))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_lua_apps_try_lock(apps);
  if (rc != H2_PAL_OK)
    return rc;
  if (!apps->ready) {
    h2_lua_apps_unlock(apps);
    return H2_PAL_ERR_INVALID_STATE;
  }
  int i = h2_lua_apps_find(apps, id);
  if (i < 0)
    rc = H2_PAL_ERR_NOT_FOUND;
  else
    *out = apps->records[i].info;
  h2_lua_apps_unlock(apps);
  return rc;
}
bool h2_lua_apps_can_install(const h2_lua_apps_t *apps) {
  if (apps == NULL || !apps->ready || !*apps->root)
    return false;
  const h2_pal_fs_vtable_t *v = apps->config.fs->vtable;
  return v && v->mkdir && v->open && v->read && v->write && v->sync &&
         v->close && v->stat && v->remove && v->rename;
}

int h2_lua_apps_publish(h2_lua_apps_t *apps, const app_record_t *record,
                        size_t position) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(&apps->json_allocator);
  if (doc == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  yyjson_mut_val *root = yyjson_mut_obj(doc), *array = yyjson_mut_arr(doc);
  bool ok = root && array;
  if (ok) {
    yyjson_mut_doc_set_root(doc, root);
    ok = yyjson_mut_obj_add_uint(doc, root, "format", 1) &&
         yyjson_mut_obj_add_val(doc, root, "apps", array);
  }
  size_t count = position == apps->count ? apps->count + 1u : apps->count;
  for (size_t i = 0; ok && i < count; ++i) {
    const app_record_t *r = i == position ? record : &apps->records[i];
    if (r->slot < 0)
      continue;
    yyjson_mut_val *value = yyjson_mut_obj(doc);
    ok = value &&
         yyjson_mut_obj_add_strcpy(doc, value, "app_id", r->info.app_id) &&
         yyjson_mut_obj_add_uint(doc, value, "slot", (uint64_t)r->slot) &&
         yyjson_mut_arr_append(array, value);
  }
  size_t len = 0;
  char *bytes =
      ok ? yyjson_mut_write_opts(doc, 0, &apps->json_allocator, &len, NULL)
         : NULL;
  int rc = bytes ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
  char tmp[H2_LUA_APP_PATH_MAX], path[H2_LUA_APP_PATH_MAX];
  if (rc == H2_PAL_OK)
    rc = h2_lua_apps_join(tmp, apps->root, "index.tmp");
  if (rc == H2_PAL_OK)
    rc = h2_lua_apps_join(path, apps->root, "index.json");
  if (rc == H2_PAL_OK)
    rc = h2_lua_apps_write(apps, tmp, bytes, len);
  if (rc == H2_PAL_OK)
    rc = h2_pal_fs_rename(apps->config.fs, tmp, path);
  if (rc == H2_PAL_OK) {
    apps->records[position] = *record;
    apps->count = count;
  }
  h2_pal_mem_free(apps->config.allocator, bytes);
  yyjson_mut_doc_free(doc);
  return rc;
}

int h2_lua_apps_remove_slot(h2_lua_apps_t *apps, const char *root) {
  app_manifest_t *m = h2_pal_mem_alloc(apps->config.allocator, sizeof(*m));
  if (m == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  char temporary[H2_LUA_APP_PATH_MAX];
  if (h2_lua_apps_join(temporary, root, "manifest.tmp") == H2_PAL_OK)
    (void)h2_pal_fs_remove(apps->config.fs, temporary);
  int rc = read_manifest(apps, root, m);
  if (rc == H2_PAL_OK) {
    for (size_t i = 0; i < m->count; ++i) {
      char path[H2_LUA_APP_PATH_MAX];
      rc = h2_lua_apps_join(path, root, m->files[i].path);
      if (rc != H2_PAL_OK)
        break;
      rc = h2_pal_fs_remove(apps->config.fs, path);
      if (rc == H2_PAL_ERR_NOT_FOUND)
        rc = H2_PAL_OK;
      if (rc != H2_PAL_OK)
        break;
      /* Removing a nonempty parent fails harmlessly; later leaves retry it. */
      for (char *p = strrchr(path, '/'); p && (size_t)(p - path) > strlen(root);
           p = strrchr(path, '/')) {
        *p = 0;
        (void)h2_pal_fs_remove(apps->config.fs, path);
      }
    }
    char path[H2_LUA_APP_PATH_MAX];
    if (rc == H2_PAL_OK &&
        h2_lua_apps_join(path, root, "manifest.json") == H2_PAL_OK)
      rc = h2_pal_fs_remove(apps->config.fs, path);
  } else if (rc == H2_PAL_ERR_NOT_FOUND)
    rc = H2_PAL_OK;
  if (rc == H2_PAL_OK)
    (void)h2_pal_fs_remove(apps->config.fs, root);
  h2_pal_mem_free(apps->config.allocator, m);
  return rc;
}
