#include "h2_memory_pref.h"

#include <stdbool.h>
#include <string.h>

#define NAME_MAX_BYTES 64u
#define KEY_MAX_BYTES 96u

typedef struct entry {
  struct entry* next;
  char key[KEY_MAX_BYTES + 1u];
  h2_pal_pref_entry_type_t type;
  size_t size;
  uint8_t value[];
} entry_t;

typedef struct space {
  struct space* next;
  char name[NAME_MAX_BYTES + 1u];
  entry_t* committed;
  bool writer;
} space_t;

typedef struct handle {
  h2_pal_pref_namespace_t api;
  h2_memory_pref_t* owner;
  space_t* space;
  entry_t* staged;
  bool writable;
  size_t cursors;
} handle_t;

typedef struct metadata {
  char key[KEY_MAX_BYTES + 1u];
  h2_pal_pref_entry_type_t type;
  size_t size;
} metadata_t;

struct h2_pal_pref_cursor {
  handle_t* owner;
  size_t count, index;
  metadata_t entries[];
};

struct h2_memory_pref {
  h2_pal_pref_api_t api;
  h2_memory_pref_config_t config;
  space_t* spaces;
  size_t namespace_count, handles;
};

static void wipe(void* data, size_t size) {
  volatile uint8_t* bytes = data;
  while (size-- != 0u) *bytes++ = 0u;
}

static bool valid_name(const char* text, size_t maximum) {
  if (text == NULL || text[0] == '\0') return false;
  for (size_t i = 0u; i <= maximum; ++i)
    if (text[i] == '\0') return true;
  return false;
}

static void free_entries(h2_memory_pref_t* p, entry_t* entries) {
  while (entries != NULL) {
    entry_t* next = entries->next;
    wipe(entries, sizeof(*entries) + entries->size);
    h2_pal_mem_free(p->config.mem, entries);
    entries = next;
  }
}

static int clone(h2_memory_pref_t* p, const entry_t* entries, entry_t** out) {
  *out = NULL;
  entry_t** tail = out;
  for (const entry_t* e = entries; e != NULL; e = e->next) {
    entry_t* copy = h2_pal_mem_alloc(p->config.mem, sizeof(*copy) + e->size);
    if (copy == NULL) {
      free_entries(p, *out);
      *out = NULL;
      return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(copy, e, sizeof(*copy) + e->size);
    copy->next = NULL;
    *tail = copy;
    tail = &copy->next;
  }
  return H2_PAL_OK;
}

static entry_t* values(handle_t* h) {
  return h->writable ? h->staged : h->space->committed;
}

static entry_t* lookup(entry_t* entries, const char* key) {
  for (entry_t* e = entries; e != NULL; e = e->next)
    if (strcmp(e->key, key) == 0) return e;
  return NULL;
}

static int read_value(handle_t* h, const char* key,
                      h2_pal_pref_entry_type_t type, const entry_t** out) {
  if (h == NULL || !valid_name(key, KEY_MAX_BYTES))
    return H2_PAL_ERR_INVALID_ARG;
  const entry_t* e = lookup(values(h), key);
  if (e == NULL) return H2_PAL_ERR_NOT_FOUND;
  if (e->type != type) return H2_PAL_ERR_FORMAT;
  *out = e;
  return H2_PAL_OK;
}

static int write_value(handle_t* h, const char* key,
                       h2_pal_pref_entry_type_t type, const void* data,
                       size_t size) {
  if (h == NULL || !valid_name(key, KEY_MAX_BYTES) ||
      (data == NULL && size != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  if (!h->writable) return H2_PAL_ERR_INVALID_STATE;
  if (size > h->owner->config.max_value_bytes ||
      size > SIZE_MAX - sizeof(entry_t))
    return H2_PAL_ERR_NO_SPACE;
  entry_t** slot = &h->staged;
  size_t count = 0u;
  for (entry_t* e = h->staged; e != NULL; e = e->next) ++count;
  while (*slot != NULL && strcmp((*slot)->key, key) != 0) slot = &(*slot)->next;
  if (*slot == NULL && count >= h->owner->config.max_entries_per_namespace)
    return H2_PAL_ERR_NO_SPACE;
  entry_t* next = h2_pal_mem_alloc(h->owner->config.mem, sizeof(*next) + size);
  if (next == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(next, 0, sizeof(*next));
  memcpy(next->key, key, strlen(key) + 1u);
  next->type = type;
  next->size = size;
  if (size != 0u) memcpy(next->value, data, size);
  entry_t* old = *slot;
  next->next = old != NULL ? old->next : NULL;
  *slot = next;
  if (old != NULL) {
    old->next = NULL;
    free_entries(h->owner, old);
  }
  return H2_PAL_OK;
}

static int get_alloc(h2_pal_pref_namespace_t* ns, const h2_pal_mem_api_t* mem,
                     const char* key, void** out, size_t* size,
                     h2_pal_pref_entry_type_t type) {
  if (out == NULL || size == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  *size = 0u;
  if (mem == NULL || mem->vtable == NULL || mem->vtable->alloc == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const entry_t* e = NULL;
  int rc = read_value(ns != NULL ? ns->user : NULL, key, type, &e);
  if (rc != H2_PAL_OK) return rc;
  void* copy = h2_pal_mem_alloc(mem, e->size != 0u ? e->size : 1u);
  if (copy == NULL) return H2_PAL_ERR_NO_MEMORY;
  if (e->size != 0u) memcpy(copy, e->value, e->size);
  *out = copy;
  *size = e->size;
  return H2_PAL_OK;
}

static int get_blob(h2_pal_pref_namespace_t* ns, const h2_pal_mem_api_t* mem,
                    const char* key, void** out, size_t* size) {
  return get_alloc(ns, mem, key, out, size, H2_PAL_PREF_ENTRY_BLOB);
}
static int set_blob(h2_pal_pref_namespace_t* ns, const char* key,
                    const void* data, size_t size) {
  return write_value(ns != NULL ? ns->user : NULL, key, H2_PAL_PREF_ENTRY_BLOB,
                     data, size);
}
static int get_string(h2_pal_pref_namespace_t* ns, const h2_pal_mem_api_t* mem,
                      const char* key, char** out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  void* data = NULL;
  size_t size = 0u;
  int rc = get_alloc(ns, mem, key, &data, &size, H2_PAL_PREF_ENTRY_STRING);
  if (rc == H2_PAL_OK) *out = data;
  return rc;
}
static int set_string(h2_pal_pref_namespace_t* ns, const char* key,
                      const char* value) {
  if (value == NULL) return H2_PAL_ERR_INVALID_ARG;
  return write_value(ns != NULL ? ns->user : NULL, key,
                     H2_PAL_PREF_ENTRY_STRING, value, strlen(value) + 1u);
}
static int get_scalar(h2_pal_pref_namespace_t* ns, const char* key, void* out,
                      size_t size, h2_pal_pref_entry_type_t type) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, size);
  const entry_t* e = NULL;
  int rc = read_value(ns != NULL ? ns->user : NULL, key, type, &e);
  if (rc == H2_PAL_OK && e->size != size) rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) memcpy(out, e->value, size);
  return rc;
}
static int get_u32(h2_pal_pref_namespace_t* ns, const char* key,
                   uint32_t* out) {
  return get_scalar(ns, key, out, sizeof(*out), H2_PAL_PREF_ENTRY_U32);
}
static int set_u32(h2_pal_pref_namespace_t* ns, const char* key,
                   uint32_t value) {
  return write_value(ns != NULL ? ns->user : NULL, key, H2_PAL_PREF_ENTRY_U32,
                     &value, sizeof(value));
}
static int get_i32(h2_pal_pref_namespace_t* ns, const char* key, int32_t* out) {
  return get_scalar(ns, key, out, sizeof(*out), H2_PAL_PREF_ENTRY_I32);
}
static int set_i32(h2_pal_pref_namespace_t* ns, const char* key,
                   int32_t value) {
  return write_value(ns != NULL ? ns->user : NULL, key, H2_PAL_PREF_ENTRY_I32,
                     &value, sizeof(value));
}
static int get_bool(h2_pal_pref_namespace_t* ns, const char* key, int* out) {
  return get_scalar(ns, key, out, sizeof(*out), H2_PAL_PREF_ENTRY_BOOL);
}
static int set_bool(h2_pal_pref_namespace_t* ns, const char* key, int value) {
  value = value != 0;
  return write_value(ns != NULL ? ns->user : NULL, key, H2_PAL_PREF_ENTRY_BOOL,
                     &value, sizeof(value));
}
static int remove_key(h2_pal_pref_namespace_t* ns, const char* key) {
  handle_t* h = ns != NULL ? ns->user : NULL;
  if (h == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!valid_name(key, KEY_MAX_BYTES)) return H2_PAL_ERR_INVALID_ARG;
  if (!h->writable) return H2_PAL_ERR_INVALID_STATE;
  entry_t** slot = &h->staged;
  while (*slot != NULL && strcmp((*slot)->key, key) != 0) slot = &(*slot)->next;
  if (*slot == NULL) return H2_PAL_ERR_NOT_FOUND;
  entry_t* old = *slot;
  *slot = old->next;
  old->next = NULL;
  free_entries(h->owner, old);
  return H2_PAL_OK;
}
static int clear_ns(h2_pal_pref_namespace_t* ns) {
  handle_t* h = ns != NULL ? ns->user : NULL;
  if (h == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!h->writable) return H2_PAL_ERR_INVALID_STATE;
  free_entries(h->owner, h->staged);
  h->staged = NULL;
  return H2_PAL_OK;
}
static int commit_ns(h2_pal_pref_namespace_t* ns) {
  handle_t* h = ns != NULL ? ns->user : NULL;
  if (h == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!h->writable) return H2_PAL_ERR_INVALID_STATE;
  entry_t* replacement = NULL;
  int rc = clone(h->owner, h->staged, &replacement);
  if (rc != H2_PAL_OK) return rc;
  free_entries(h->owner, h->space->committed);
  h->space->committed = replacement;
  return H2_PAL_OK;
}

static int iterate_close(h2_pal_pref_namespace_t* ns,
                         h2_pal_pref_cursor_t** cursor) {
  handle_t* h = ns != NULL ? ns->user : NULL;
  if (h == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (cursor == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (*cursor == NULL) return H2_PAL_OK;
  if ((*cursor)->owner != h) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_mem_free(h->owner->config.mem, *cursor);
  *cursor = NULL;
  --h->cursors;
  return H2_PAL_OK;
}
static int iterate(h2_pal_pref_namespace_t* ns, h2_pal_pref_cursor_t** cursor,
                   h2_pal_pref_entry_t* out) {
  handle_t* h = ns != NULL ? ns->user : NULL;
  if (h == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (cursor == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (*cursor == NULL) {
    if (h->cursors != 0u) return H2_PAL_ERR_BUSY;
    size_t count = 0u;
    for (entry_t* e = values(h); e != NULL; e = e->next) ++count;
    if (count == 0u) return H2_PAL_ERR_NOT_FOUND;
    if (count > (SIZE_MAX - sizeof(**cursor)) / sizeof(metadata_t))
      return H2_PAL_ERR_NO_SPACE;
    h2_pal_pref_cursor_t* c = h2_pal_mem_alloc(
        h->owner->config.mem, sizeof(*c) + count * sizeof(metadata_t));
    if (c == NULL) return H2_PAL_ERR_NO_MEMORY;
    c->owner = h;
    c->count = count;
    c->index = 0u;
    size_t i = 0u;
    for (entry_t* e = values(h); e != NULL; e = e->next) {
      memcpy(c->entries[i].key, e->key, sizeof(e->key));
      c->entries[i].type = e->type;
      c->entries[i++].size = e->size;
    }
    *cursor = c;
    ++h->cursors;
  }
  h2_pal_pref_cursor_t* c = *cursor;
  if (c->owner != h) return H2_PAL_ERR_INVALID_ARG;
  if (c->index == c->count) {
    (void)iterate_close(ns, cursor);
    return H2_PAL_ERR_NOT_FOUND;
  }
  metadata_t* e = &c->entries[c->index++];
  *out = (h2_pal_pref_entry_t){e->key, e->type, e->size};
  return H2_PAL_OK;
}
static int close_ns(h2_pal_pref_namespace_t* ns) {
  handle_t* h = ns != NULL ? ns->user : NULL;
  if (h == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (h->cursors != 0u) return H2_PAL_ERR_INVALID_STATE;
  if (h->writable) {
    h->space->writer = false;
    free_entries(h->owner, h->staged);
  }
  --h->owner->handles;
  h2_pal_mem_free(h->owner->config.mem, h);
  return H2_PAL_OK;
}
static int open_ns(void* user, const char* name, h2_pal_pref_open_mode_t mode,
                   h2_pal_pref_namespace_t** out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  h2_memory_pref_t* p = user;
  if (p == NULL || !valid_name(name, NAME_MAX_BYTES) ||
      (mode != H2_PAL_PREF_OPEN_READ_ONLY &&
       mode != H2_PAL_PREF_OPEN_READ_WRITE))
    return H2_PAL_ERR_INVALID_ARG;
  if (p->handles >= p->config.max_handles) return H2_PAL_ERR_NO_SPACE;
  space_t* space = p->spaces;
  while (space != NULL && strcmp(space->name, name) != 0) space = space->next;
  if (space == NULL && mode == H2_PAL_PREF_OPEN_READ_ONLY)
    return H2_PAL_ERR_NOT_FOUND;
  if (space == NULL && p->namespace_count >= p->config.max_namespaces)
    return H2_PAL_ERR_NO_SPACE;
  if (space != NULL && space->writer && mode == H2_PAL_PREF_OPEN_READ_WRITE)
    return H2_PAL_ERR_BUSY;
  handle_t* h = h2_pal_mem_alloc(p->config.mem, sizeof(*h));
  if (h == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(h, 0, sizeof(*h));
  int rc = H2_PAL_OK;
  if (mode == H2_PAL_PREF_OPEN_READ_WRITE && space != NULL)
    rc = clone(p, space->committed, &h->staged);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(p->config.mem, h);
    return rc;
  }
  if (space == NULL) {
    space = h2_pal_mem_alloc(p->config.mem, sizeof(*space));
    if (space == NULL) {
      h2_pal_mem_free(p->config.mem, h);
      return H2_PAL_ERR_NO_MEMORY;
    }
    memset(space, 0, sizeof(*space));
    memcpy(space->name, name, strlen(name) + 1u);
    space->next = p->spaces;
    p->spaces = space;
    ++p->namespace_count;
  }
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
  h->owner = p;
  h->space = space;
  h->writable = mode == H2_PAL_PREF_OPEN_READ_WRITE;
  if (h->writable) space->writer = true;
  ++p->handles;
  *out = &h->api;
  return H2_PAL_OK;
}

h2_pal_result_t h2_memory_pref_create(const h2_memory_pref_config_t* config,
                                      h2_memory_pref_t** out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (config == NULL || config->mem == NULL || config->mem->vtable == NULL ||
      config->mem->vtable->alloc == NULL || config->mem->vtable->free == NULL ||
      config->max_namespaces == 0u || config->max_entries_per_namespace == 0u ||
      config->max_value_bytes == 0u || config->max_handles == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_memory_pref_t* p = h2_pal_mem_alloc(config->mem, sizeof(*p));
  if (p == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(p, 0, sizeof(*p));
  p->config = *config;
  static const h2_pal_pref_vtable_t vtable = {.open = open_ns};
  p->api = (h2_pal_pref_api_t){p, &vtable};
  *out = p;
  return H2_PAL_OK;
}
const h2_pal_pref_api_t* h2_memory_pref_api(h2_memory_pref_t* owner) {
  return owner != NULL ? &owner->api : NULL;
}
h2_pal_result_t h2_memory_pref_destroy(h2_memory_pref_t** owner) {
  if (owner == NULL || *owner == NULL) return H2_PAL_OK;
  h2_memory_pref_t* p = *owner;
  if (p->handles != 0u) return H2_PAL_ERR_INVALID_STATE;
  space_t* space = p->spaces;
  while (space != NULL) {
    space_t* next = space->next;
    free_entries(p, space->committed);
    wipe(space, sizeof(*space));
    h2_pal_mem_free(p->config.mem, space);
    space = next;
  }
  const h2_pal_mem_api_t* mem = p->config.mem;
  wipe(p, sizeof(*p));
  h2_pal_mem_free(mem, p);
  *owner = NULL;
  return H2_PAL_OK;
}
