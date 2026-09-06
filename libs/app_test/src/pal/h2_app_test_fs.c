#include "h2_app_test_fs.h"
#include <stdbool.h>
#include <string.h>
typedef struct file_entry {
  char path[H2_APP_TEST_FS_PATH_MAX + 1u];
  uint8_t *data;
  size_t size;
  bool opened;
} file_entry_t;
typedef struct store {
  file_entry_t files[H2_APP_TEST_FS_FILES_MAX];
} store_t;
struct h2_pal_fs_file {
  h2_app_test_fs_t *owner;
  file_entry_t *entry;
  size_t offset;
  h2_pal_fs_open_mode_t mode;
};
static bool valid_path(const char *p) {
  if (!p || !p[0])
    return false;
  size_t i = 0;
  while (i <= H2_APP_TEST_FS_PATH_MAX && p[i])
    ++i;
  return i <= H2_APP_TEST_FS_PATH_MAX;
}
static file_entry_t *find(h2_app_test_fs_t *f, const char *path) {
  store_t *s = f->implementation;
  for (size_t i = 0; i < H2_APP_TEST_FS_FILES_MAX; ++i)
    if (s->files[i].path[0] && !strcmp(s->files[i].path, path))
      return &s->files[i];
  return NULL;
}
static bool valid_file(h2_app_test_fs_t *f, h2_pal_fs_file_t *h) {
  return h && h->owner == f && h->entry->opened;
}
static int open_file(void *u, const char *path, h2_pal_fs_open_mode_t mode,
                     h2_pal_fs_file_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  h2_app_test_fs_t *f = u;
  if (!valid_path(path) ||
      (mode != H2_PAL_FS_OPEN_READ && mode != H2_PAL_FS_OPEN_WRITE_TRUNCATE))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_app_test_fault_take(&f->open);
  if (rc)
    return rc;
  file_entry_t *e = find(f, path);
  if (e && e->opened)
    return H2_PAL_ERR_INVALID_STATE;
  if (!e && mode == H2_PAL_FS_OPEN_READ)
    return H2_PAL_ERR_NOT_FOUND;
  if (!e) {
    store_t *s = f->implementation;
    for (size_t i = 0; i < H2_APP_TEST_FS_FILES_MAX; ++i)
      if (!s->files[i].path[0]) {
        e = &s->files[i];
        break;
      }
  }
  if (!e)
    return H2_PAL_ERR_NO_SPACE;
  h2_pal_fs_file_t *h = h2_pal_mem_alloc(f->mem, sizeof(*h));
  if (!h)
    return H2_PAL_ERR_NO_MEMORY;
  if (mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE) {
    h2_pal_mem_free(f->mem, e->data);
    e->data = NULL;
    e->size = 0;
  }
  strcpy(e->path, path);
  e->opened = true;
  *h = (h2_pal_fs_file_t){f, e, 0, mode};
  ++f->active_handles;
  *out = h;
  return 0;
}
static int read_file(void *u, h2_pal_fs_file_t *h, void *data, size_t len,
                     size_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  h2_app_test_fs_t *f = u;
  if (!valid_file(f, h) || h->mode != H2_PAL_FS_OPEN_READ || (!data && len))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_app_test_fault_take(&f->read);
  if (rc)
    return rc;
  size_t remaining =
      h->offset < h->entry->size ? h->entry->size - h->offset : 0;
  size_t n = len < remaining ? len : remaining;
  if (f->max_read_bytes && n > f->max_read_bytes)
    n = f->max_read_bytes;
  if (n)
    memcpy(data, h->entry->data + h->offset, n);
  h->offset += n;
  *out = n;
  return 0;
}
static int write_file(void *u, h2_pal_fs_file_t *h, const void *data,
                      size_t len, size_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  h2_app_test_fs_t *f = u;
  if (!valid_file(f, h) || h->mode != H2_PAL_FS_OPEN_WRITE_TRUNCATE ||
      (!data && len))
    return H2_PAL_ERR_INVALID_ARG;
  if (h->offset > f->max_file_bytes || len > f->max_file_bytes - h->offset)
    return H2_PAL_ERR_NO_SPACE;
  int rc = h2_app_test_fault_take(&f->write);
  if (rc)
    return rc;
  if (!len)
    return 0;
  size_t end = h->offset + len;
  file_entry_t *e = h->entry;
  if (end > e->size) {
    uint8_t *next = h2_pal_mem_alloc(f->mem, end);
    if (!next)
      return H2_PAL_ERR_NO_MEMORY;
    memset(next, 0, end);
    if (e->size)
      memcpy(next, e->data, e->size);
    h2_pal_mem_free(f->mem, e->data);
    e->data = next;
    e->size = end;
  }
  memcpy(e->data + h->offset, data, len);
  h->offset = end;
  *out = len;
  return 0;
}
static int seek_file(void *u, h2_pal_fs_file_t *h, uint64_t offset) {
  h2_app_test_fs_t *f = u;
  if (!valid_file(f, h))
    return H2_PAL_ERR_INVALID_ARG;
  if (offset > f->max_file_bytes)
    return H2_PAL_ERR_NO_SPACE;
  h->offset = (size_t)offset;
  return 0;
}
static int sync_file(void *u, h2_pal_fs_file_t *h) {
  h2_app_test_fs_t *f = u;
  if (!valid_file(f, h))
    return H2_PAL_ERR_INVALID_ARG;
  return h2_app_test_fault_take(&f->sync);
}
static int close_file(void *u, h2_pal_fs_file_t *h) {
  h2_app_test_fs_t *f = u;
  if (!valid_file(f, h))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_app_test_fault_take(&f->close);
  if (rc)
    return rc;
  h->entry->opened = false;
  --f->active_handles;
  h2_pal_mem_free(f->mem, h);
  return 0;
}
static int stat_file(void *u, const char *path, h2_pal_fs_stat_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  h2_app_test_fs_t *f = u;
  if (!valid_path(path))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_app_test_fault_take(&f->stat);
  if (rc)
    return rc;
  file_entry_t *e = find(f, path);
  if (!e)
    return H2_PAL_ERR_NOT_FOUND;
  out->size = e->size;
  return 0;
}
static int remove_file(void *u, const char *path) {
  h2_app_test_fs_t *f = u;
  if (!valid_path(path))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_app_test_fault_take(&f->remove);
  if (rc)
    return rc;
  file_entry_t *e = find(f, path);
  if (!e)
    return H2_PAL_ERR_NOT_FOUND;
  if (e->opened)
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_mem_free(f->mem, e->data);
  memset(e, 0, sizeof(*e));
  return 0;
}
static int rename_file(void *u, const char *old, const char *next) {
  h2_app_test_fs_t *f = u;
  if (!valid_path(old) || !valid_path(next))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_app_test_fault_take(&f->rename);
  if (rc)
    return rc;
  file_entry_t *e = find(f, old), *to = find(f, next);
  if (!e)
    return H2_PAL_ERR_NOT_FOUND;
  if (e->opened || (to && to->opened))
    return H2_PAL_ERR_INVALID_STATE;
  if (e == to)
    return 0;
  if (to) {
    h2_pal_mem_free(f->mem, to->data);
    memset(to, 0, sizeof(*to));
  }
  strcpy(e->path, next);
  return 0;
}
static int unsupported_path(void *u, const char *p) {
  (void)u;
  (void)p;
  return H2_PAL_ERR_UNSUPPORTED;
}
static const h2_pal_fs_vtable_t vtable = {.mkdir = unsupported_path,
                                          .clear = unsupported_path,
                                          .open = open_file,
                                          .read = read_file,
                                          .write = write_file,
                                          .seek = seek_file,
                                          .sync = sync_file,
                                          .close = close_file,
                                          .stat = stat_file,
                                          .remove = remove_file,
                                          .rename = rename_file};
h2_pal_result_t h2_app_test_fs_init(h2_app_test_fs_t *f,
                                    const h2_pal_mem_api_t *m) {
  if (!f)
    return H2_PAL_ERR_INVALID_ARG;
  memset(f, 0, sizeof(*f));
  if (!m || !m->vtable || !m->vtable->alloc || !m->vtable->free)
    return H2_PAL_ERR_INVALID_ARG;
  f->implementation = h2_pal_mem_alloc(m, sizeof(store_t));
  if (!f->implementation)
    return H2_PAL_ERR_NO_MEMORY;
  memset(f->implementation, 0, sizeof(store_t));
  f->mem = m;
  f->max_file_bytes = 1024u * 1024u;
  f->api = (h2_pal_fs_api_t){f, &vtable};
  return 0;
}
h2_pal_result_t h2_app_test_fs_deinit(h2_app_test_fs_t *f) {
  if (!f || !f->implementation)
    return 0;
  if (f->active_handles)
    return H2_PAL_ERR_INVALID_STATE;
  store_t *s = f->implementation;
  for (size_t i = 0; i < H2_APP_TEST_FS_FILES_MAX; ++i)
    h2_pal_mem_free(f->mem, s->files[i].data);
  h2_pal_mem_free(f->mem, s);
  memset(f, 0, sizeof(*f));
  return 0;
}
