#include "h2_app_test_pref.h"
#include <stdbool.h>
#include <string.h>
typedef struct entry {
  char key[H2_APP_TEST_PREF_NAME_MAX + 1u];
  h2_pal_pref_entry_type_t type;
  size_t size;
  bool present, dirty;
  uint8_t value[H2_APP_TEST_PREF_VALUE_MAX];
} entry_t;
typedef struct space {
  char name[H2_APP_TEST_PREF_NAME_MAX + 1u];
  entry_t *entries;
  bool writer;
} space_t;
typedef struct store {
  space_t spaces[H2_APP_TEST_PREF_NAMESPACES_MAX];
} store_t;
typedef struct handle {
  h2_pal_pref_namespace_t api;
  h2_app_test_pref_t *owner;
  space_t *space;
  entry_t *staged;
} handle_t;
/* Bounded string validation must not read beyond the first NUL. */
static size_t length(const char *s, size_t cap) {
  size_t n = 0;
  if (!s)
    return cap;
  while (n < cap && s[n])
    ++n;
  return n;
}
static entry_t *entries(handle_t *h) {
  return h->staged ? h->staged : h->space->entries;
}
static entry_t *lookup(entry_t *es, const char *key) {
  for (size_t i = 0; i < H2_APP_TEST_PREF_ENTRIES_MAX; ++i)
    if (es[i].key[0] && !strcmp(es[i].key, key))
      return &es[i];
  return NULL;
}
static int read_value(handle_t *h, const char *key,
                      h2_pal_pref_entry_type_t type, entry_t **out) {
  if (!key ||
      length(key, H2_APP_TEST_PREF_NAME_MAX + 1u) > H2_APP_TEST_PREF_NAME_MAX ||
      !key[0])
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_app_test_fault_take(&h->owner->read);
  if (rc)
    return rc;
  entry_t *e = lookup(entries(h), key);
  if (!e || !e->present)
    return H2_PAL_ERR_NOT_FOUND;
  if (e->type != type)
    return H2_PAL_ERR_FORMAT;
  *out = e;
  return 0;
}
static int write_value(handle_t *h, const char *key,
                       h2_pal_pref_entry_type_t type, const void *value,
                       size_t size) {
  if (!key || !key[0] ||
      length(key, H2_APP_TEST_PREF_NAME_MAX + 1u) > H2_APP_TEST_PREF_NAME_MAX ||
      (!value && size))
    return H2_PAL_ERR_INVALID_ARG;
  if (size > H2_APP_TEST_PREF_VALUE_MAX)
    return H2_PAL_ERR_NO_SPACE;
  if (!h->staged)
    return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_app_test_fault_take(&h->owner->write);
  if (rc)
    return rc;
  entry_t *e = lookup(h->staged, key);
  if (!e)
    for (size_t i = 0; i < H2_APP_TEST_PREF_ENTRIES_MAX; ++i)
      if (!h->staged[i].key[0]) {
        e = &h->staged[i];
        break;
      }
  if (!e)
    return H2_PAL_ERR_NO_SPACE;
  memset(e, 0, sizeof(*e));
  strcpy(e->key, key);
  e->type = type;
  e->size = size;
  e->present = true;
  e->dirty = true;
  if (size)
    memcpy(e->value, value, size);
  return 0;
}
static int close_ns(h2_pal_pref_namespace_t *ns) {
  handle_t *h = ns->user;
  h2_app_test_pref_t *p = h->owner;
  if (h->staged) {
    h->space->writer = false;
    h2_pal_mem_free(p->mem, h->staged);
  }
  --p->active_handles;
  h2_pal_mem_free(p->mem, h);
  return 0;
}
static int get_alloc(h2_pal_pref_namespace_t *ns, const h2_pal_mem_api_t *mem,
                     const char *key, void **out, size_t *size,
                     h2_pal_pref_entry_type_t type) {
  if (!out || !size)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  *size = 0;
  if (!mem || !mem->vtable || !mem->vtable->alloc)
    return H2_PAL_ERR_INVALID_ARG;
  entry_t *e = NULL;
  int rc = read_value(ns->user, key, type, &e);
  if (rc)
    return rc;
  void *data = h2_pal_mem_alloc(mem, e->size ? e->size : 1u);
  if (!data)
    return H2_PAL_ERR_NO_MEMORY;
  if (e->size)
    memcpy(data, e->value, e->size);
  *out = data;
  *size = e->size;
  return 0;
}
static int get_blob(h2_pal_pref_namespace_t *n, const h2_pal_mem_api_t *m,
                    const char *k, void **o, size_t *s) {
  return get_alloc(n, m, k, o, s, H2_PAL_PREF_ENTRY_BLOB);
}
static int set_blob(h2_pal_pref_namespace_t *n, const char *k, const void *v,
                    size_t s) {
  return write_value(n->user, k, H2_PAL_PREF_ENTRY_BLOB, v, s);
}
static int get_string(h2_pal_pref_namespace_t *n, const h2_pal_mem_api_t *m,
                      const char *k, char **o) {
  if (!o)
    return H2_PAL_ERR_INVALID_ARG;
  *o = NULL;
  void *v = NULL;
  size_t s = 0;
  int rc = get_alloc(n, m, k, &v, &s, H2_PAL_PREF_ENTRY_STRING);
  if (!rc)
    *o = v;
  return rc;
}
static int set_string(h2_pal_pref_namespace_t *n, const char *k,
                      const char *v) {
  if (!v)
    return H2_PAL_ERR_INVALID_ARG;
  size_t size = length(v, H2_APP_TEST_PREF_VALUE_MAX);
  if (size == H2_APP_TEST_PREF_VALUE_MAX)
    return H2_PAL_ERR_NO_SPACE;
  return write_value(n->user, k, H2_PAL_PREF_ENTRY_STRING, v, size + 1u);
}
static int get_u32(h2_pal_pref_namespace_t *n, const char *k, uint32_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  entry_t *e = NULL;
  int rc = read_value(n->user, k, H2_PAL_PREF_ENTRY_U32, &e);
  if (!rc)
    memcpy(out, e->value, sizeof(*out));
  return rc;
}
static int set_u32(h2_pal_pref_namespace_t *n, const char *k, uint32_t value) {
  return write_value(n->user, k, H2_PAL_PREF_ENTRY_U32, &value, sizeof(value));
}
static int get_i32(h2_pal_pref_namespace_t *n, const char *k, int32_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  entry_t *e = NULL;
  int rc = read_value(n->user, k, H2_PAL_PREF_ENTRY_I32, &e);
  if (!rc)
    memcpy(out, e->value, sizeof(*out));
  return rc;
}
static int set_i32(h2_pal_pref_namespace_t *n, const char *k, int32_t value) {
  return write_value(n->user, k, H2_PAL_PREF_ENTRY_I32, &value, sizeof(value));
}
static int get_bool(h2_pal_pref_namespace_t *n, const char *k, int *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  entry_t *e = NULL;
  int rc = read_value(n->user, k, H2_PAL_PREF_ENTRY_BOOL, &e);
  if (!rc)
    memcpy(out, e->value, sizeof(*out));
  return rc;
}
static int set_bool(h2_pal_pref_namespace_t *n, const char *k, int value) {
  value = !!value;
  return write_value(n->user, k, H2_PAL_PREF_ENTRY_BOOL, &value, sizeof(value));
}
static int remove_key(h2_pal_pref_namespace_t *n, const char *k) {
  handle_t *h = n->user;
  if (!k || !k[0] ||
      length(k, H2_APP_TEST_PREF_NAME_MAX + 1u) > H2_APP_TEST_PREF_NAME_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  if (!h->staged)
    return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_app_test_fault_take(&h->owner->write);
  if (rc)
    return rc;
  entry_t *e = lookup(h->staged, k);
  if (!e || !e->present)
    return H2_PAL_ERR_NOT_FOUND;
  e->present = false;
  e->dirty = true;
  return 0;
}
static int clear_ns(h2_pal_pref_namespace_t *n) {
  handle_t *h = n->user;
  if (!h->staged)
    return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_app_test_fault_take(&h->owner->write);
  if (rc)
    return rc;
  for (size_t i = 0; i < H2_APP_TEST_PREF_ENTRIES_MAX; ++i)
    if (h->staged[i].present) {
      h->staged[i].present = false;
      h->staged[i].dirty = true;
    }
  return 0;
}
static int commit_ns(h2_pal_pref_namespace_t *n) {
  handle_t *h = n->user;
  h2_app_test_pref_t *p = h->owner;
  if (!h->staged)
    return H2_PAL_ERR_INVALID_STATE;
  if (length(p->commit_namespace, sizeof(p->commit_namespace)) ==
          sizeof(p->commit_namespace) ||
      length(p->commit_key, sizeof(p->commit_key)) == sizeof(p->commit_key))
    return H2_PAL_ERR_INVALID_ARG;
  entry_t *e = p->commit_key[0] ? lookup(h->staged, p->commit_key) : NULL;
  if ((!p->commit_namespace[0] ||
       !strcmp(p->commit_namespace, h->space->name)) &&
      (!p->commit_key[0] || (e && e->dirty))) {
    int rc = h2_app_test_fault_take(&p->commit);
    if (rc)
      return rc;
  }
  for (size_t i = 0; i < H2_APP_TEST_PREF_ENTRIES_MAX; ++i) {
    h->staged[i].dirty = false;
    if (!h->staged[i].present)
      memset(&h->staged[i], 0, sizeof(entry_t));
  }
  memcpy(h->space->entries, h->staged,
         H2_APP_TEST_PREF_ENTRIES_MAX * sizeof(entry_t));
  return 0;
}
static int iterate(h2_pal_pref_namespace_t *n, h2_pal_pref_cursor_t **c,
                   h2_pal_pref_entry_t *out) {
  (void)n;
  if (c)
    *c = NULL;
  if (out)
    memset(out, 0, sizeof(*out));
  return H2_PAL_ERR_UNSUPPORTED;
}
static int iterate_close(h2_pal_pref_namespace_t *n, h2_pal_pref_cursor_t **c) {
  (void)n;
  if (c)
    *c = NULL;
  return 0;
}
static int open_ns(void *u, const char *name, h2_pal_pref_open_mode_t mode,
                   h2_pal_pref_namespace_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  h2_app_test_pref_t *p = u;
  if (!p->implementation)
    return H2_PAL_ERR_INVALID_STATE;
  if (!name || !name[0] ||
      length(name, H2_APP_TEST_PREF_NAME_MAX + 1u) >
          H2_APP_TEST_PREF_NAME_MAX ||
      (mode != H2_PAL_PREF_OPEN_READ_ONLY &&
       mode != H2_PAL_PREF_OPEN_READ_WRITE))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_app_test_fault_take(&p->open);
  if (rc)
    return rc;
  if (p->active_handles >= H2_APP_TEST_PREF_HANDLES_MAX)
    return H2_PAL_ERR_NO_SPACE;
  store_t *store = p->implementation;
  space_t *space = NULL, *empty = NULL;
  for (size_t i = 0; i < H2_APP_TEST_PREF_NAMESPACES_MAX; ++i) {
    space_t *s = &store->spaces[i];
    if (s->name[0] && !strcmp(s->name, name))
      space = s;
    else if (!s->name[0] && !empty)
      empty = s;
  }
  if (!space && mode == H2_PAL_PREF_OPEN_READ_ONLY)
    return H2_PAL_ERR_NOT_FOUND;
  if (!space && !empty)
    return H2_PAL_ERR_NO_SPACE;
  if (space && space->writer && mode == H2_PAL_PREF_OPEN_READ_WRITE)
    return H2_PAL_ERR_INVALID_STATE;
  handle_t *h = h2_pal_mem_alloc(p->mem, sizeof(*h));
  if (!h)
    return H2_PAL_ERR_NO_MEMORY;
  memset(h, 0, sizeof(*h));
  size_t bytes = H2_APP_TEST_PREF_ENTRIES_MAX * sizeof(entry_t);
  if (mode == H2_PAL_PREF_OPEN_READ_WRITE) {
    h->staged = h2_pal_mem_alloc(p->mem, bytes);
    if (!h->staged) {
      h2_pal_mem_free(p->mem, h);
      return H2_PAL_ERR_NO_MEMORY;
    }
  }
  if (!space) {
    space = empty;
    space->entries = h2_pal_mem_alloc(p->mem, bytes);
    if (!space->entries) {
      h2_pal_mem_free(p->mem, h->staged);
      h2_pal_mem_free(p->mem, h);
      return H2_PAL_ERR_NO_MEMORY;
    }
    memset(space->entries, 0, bytes);
    strcpy(space->name, name);
  }
  if (h->staged) {
    memcpy(h->staged, space->entries, bytes);
    space->writer = true;
  }
  h->owner = p;
  h->space = space;
  h->api = (h2_pal_pref_namespace_t){.user = h,
                                     .close = close_ns,
                                     .get_blob = get_blob,
                                     .set_blob = set_blob,
                                     .get_string = get_string,
                                     .set_string = set_string,
                                     .get_u32 = get_u32,
                                     .set_u32 = set_u32,
                                     .get_i32 = get_i32,
                                     .set_i32 = set_i32,
                                     .get_bool = get_bool,
                                     .set_bool = set_bool,
                                     .remove = remove_key,
                                     .clear = clear_ns,
                                     .commit = commit_ns,
                                     .iterate = iterate,
                                     .iterate_close = iterate_close};
  ++p->active_handles;
  *out = &h->api;
  return 0;
}
static const h2_pal_pref_vtable_t vtable = {.open = open_ns};
int h2_app_test_pref_init(h2_app_test_pref_t *p, const h2_pal_mem_api_t *m) {
  if (!p)
    return H2_PAL_ERR_INVALID_ARG;
  memset(p, 0, sizeof(*p));
  if (!m || !m->vtable || !m->vtable->alloc || !m->vtable->free)
    return H2_PAL_ERR_INVALID_ARG;
  p->implementation = h2_pal_mem_alloc(m, sizeof(store_t));
  if (!p->implementation)
    return H2_PAL_ERR_NO_MEMORY;
  memset(p->implementation, 0, sizeof(store_t));
  p->mem = m;
  p->api = (h2_pal_pref_api_t){p, &vtable};
  return 0;
}
int h2_app_test_pref_deinit(h2_app_test_pref_t *p) {
  if (!p || !p->implementation)
    return 0;
  if (p->active_handles)
    return H2_PAL_ERR_INVALID_STATE;
  store_t *s = p->implementation;
  for (size_t i = 0; i < H2_APP_TEST_PREF_NAMESPACES_MAX; ++i)
    h2_pal_mem_free(p->mem, s->spaces[i].entries);
  h2_pal_mem_free(p->mem, s);
  memset(p, 0, sizeof(*p));
  return 0;
}
