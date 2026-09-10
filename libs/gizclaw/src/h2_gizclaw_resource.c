#include "h2_gizclaw_resource.h"
#include "h2_gizclaw_response_internal.h"
#include "h2_runtime.h"
#include <string.h>

struct h2_gizclaw_resource {
  h2_gizclaw_resource_config_t config;
  h2_pal_mutex_t *mutex;
  h2_gizclaw_resource_snapshot_t snapshot;
  uint8_t *data;
  uint64_t deadline;
};
static h2_pal_result_t lock(h2_gizclaw_resource_t *r) {
  return h2_pal_mutex_lock(r->config.sync, r->mutex);
}
static void unlock(h2_gizclaw_resource_t *r) {
  (void)h2_pal_mutex_unlock(r->config.sync, r->mutex);
}
static void changed(h2_gizclaw_resource_t *r) {
  ++r->snapshot.revision;
  if (r->config.runtime != NULL)
    h2_runtime_notify(r->config.runtime);
}
static h2_gizclaw_str_t str(const char *s) {
  return (h2_gizclaw_str_t){s, s == NULL ? 0u : strlen(s)};
}
static bool same(const char *a, const char *b) {
  return a != NULL && b != NULL && strcmp(a, b) == 0;
}
static char *copy_string(const h2_pal_mem_api_t *mem, const char *s) {
  if (s == NULL)
    return NULL;
  size_t n = strlen(s) + 1u;
  char *out = h2_pal_mem_alloc(mem, n);
  if (out != NULL)
    memcpy(out, s, n);
  return out;
}
static h2_gizclaw_owned_text_t copy_text(const h2_pal_mem_api_t *mem,
                                         h2_gizclaw_owned_text_t s) {
  if (s.data == NULL)
    return (h2_gizclaw_owned_text_t){0};
  char *out = h2_pal_mem_alloc(mem, s.len + 1u);
  if (out != NULL) {
    memcpy(out, s.data, s.len);
    out[s.len] = 0;
  }
  return (h2_gizclaw_owned_text_t){out, s.len};
}
static void copy_contact(const h2_pal_mem_api_t *mem,
                         const h2_gizclaw_contact_t *src,
                         h2_gizclaw_contact_t *dst) {
  *dst = *src;
  dst->name = copy_string(mem, src->name);
  dst->display_name = copy_string(mem, src->display_name);
  dst->phone_number = copy_string(mem, src->phone_number);
  dst->created_at = copy_string(mem, src->created_at);
  dst->updated_at = copy_string(mem, src->updated_at);
}
static void copy_group(const h2_pal_mem_api_t *mem,
                       const h2_gizclaw_friend_group_t *src,
                       h2_gizclaw_friend_group_t *dst) {
  *dst = *src;
  dst->name = copy_string(mem, src->name);
  dst->display_name = copy_string(mem, src->display_name);
  dst->description = copy_string(mem, src->description);
  dst->workspace_name = copy_string(mem, src->workspace_name);
}
static h2_pal_result_t copy_snapshot(const h2_gizclaw_resource_snapshot_t *src,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_resource_snapshot_t *out) {
  h2_gizclaw_resp_arena_t arena;
  h2_pal_result_t rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resource_snapshot_t dst = *src;
  const h2_pal_mem_api_t *mem = &arena.allocator;
  if (src->kind == H2_GIZCLAW_RESOURCE_CONTACTS) {
    size_t count = src->data.contacts.count;
    dst.data.contacts.items = NULL;
    if (count != 0u) {
      dst.data.contacts.items =
          h2_pal_mem_alloc(mem, count * sizeof(h2_gizclaw_contact_t));
      if (dst.data.contacts.items != NULL)
        for (size_t i = 0; i < count; ++i)
          copy_contact(mem, &src->data.contacts.items[i],
                       &dst.data.contacts.items[i]);
    }
    dst.data.contacts.next_cursor =
        copy_string(mem, src->data.contacts.next_cursor);
  }
  if (src->kind == H2_GIZCLAW_RESOURCE_GROUPS) {
    size_t count = src->data.groups.count;
    dst.data.groups.items = NULL;
    if (count != 0u) {
      dst.data.groups.items =
          h2_pal_mem_alloc(mem, count * sizeof(h2_gizclaw_friend_group_t));
      if (dst.data.groups.items != NULL)
        for (size_t i = 0; i < count; ++i)
          copy_group(mem, &src->data.groups.items[i],
                     &dst.data.groups.items[i]);
    }
    dst.data.groups.next_cursor =
        copy_string(mem, src->data.groups.next_cursor);
  }
  if (src->kind == H2_GIZCLAW_RESOURCE_APP_CONFIG) {
    const h2_gizclaw_app_config_snapshot_t *a = &src->data.app_config;
    h2_gizclaw_app_config_snapshot_t *b = &dst.data.app_config;
    b->items = NULL;
    if (a->count) {
      b->items = h2_pal_mem_alloc(mem, a->count * sizeof(*b->items));
      if (b->items != NULL)
        for (size_t i = 0; i < a->count; ++i) {
          b->items[i].key = copy_string(mem, a->items[i].key);
          b->items[i].value = copy_text(mem, a->items[i].value);
        }
    }
    b->runtime_profile_name = copy_string(mem, a->runtime_profile_name);
    b->runtime_profile_revision = copy_string(mem, a->runtime_profile_revision);
  }
  rc = h2_gizclaw_resp_arena_end(&arena, H2_PAL_OK);
  if (rc == H2_PAL_OK)
    *out = dst;
  return rc;
}

h2_pal_result_t
h2_gizclaw_resource_create(const h2_gizclaw_resource_config_t *c,
                           h2_gizclaw_resource_t **out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (c == NULL || c->service == NULL || c->mem == NULL || c->sync == NULL ||
      c->time == NULL || c->kind < H2_GIZCLAW_RESOURCE_CONTACTS ||
      c->kind > H2_GIZCLAW_RESOURCE_FIRMWARE ||
      (c->kind == H2_GIZCLAW_RESOURCE_FIRMWARE && c->firmware_channel <= 0))
    return H2_PAL_ERR_INVALID_ARG;
  if (c->kind != H2_GIZCLAW_RESOURCE_FIRMWARE &&
      (c->max_items == 0u ||
       c->max_items > SIZE_MAX / sizeof(h2_gizclaw_contact_t) ||
       c->max_items > SIZE_MAX / sizeof(h2_gizclaw_friend_group_t) ||
       c->max_items > SIZE_MAX / sizeof(h2_gizclaw_app_config_entry_t) ||
       c->page_size == 0u ||
       c->page_size > (c->kind == H2_GIZCLAW_RESOURCE_APP_CONFIG
                            ? H2_GIZCLAW_APP_CONFIG_PAGE_MAX_ITEMS
                            : H2_GIZCLAW_CONTACT_PAGE_MAX_ITEMS) ||
       c->storage_bytes == 0u))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_resource_t *r = h2_pal_mem_alloc(c->mem, sizeof(*r));
  if (r == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(r, 0, sizeof(*r));
  r->config = *c;
  if (c->kind == H2_GIZCLAW_RESOURCE_FIRMWARE)
    r->config.storage_bytes = 1u; /* Inline snapshot; no arena payload. */
  r->snapshot.kind = c->kind;
  r->snapshot.stale = true;
  h2_pal_mutex_config_t mc = {.name = "gizclaw-resource", .allocator = c->mem};
  h2_pal_result_t rc = h2_pal_mutex_create(c->sync, &mc, &r->mutex);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(c->mem, r);
    return rc;
  }
  *out = r;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_resource_destroy(h2_gizclaw_resource_t **ptr) {
  if (ptr == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_resource_t *r = *ptr;
  if (r == NULL)
    return H2_PAL_OK;
  h2_pal_result_t rc = lock(r);
  if (rc != H2_PAL_OK)
    return rc;
  bool busy = r->snapshot.busy;
  unlock(r);
  if (busy)
    return H2_PAL_ERR_BUSY;
  rc = h2_pal_mutex_destroy(r->config.sync, r->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  h2_pal_mem_free(r->config.mem, r->data);
  h2_pal_mem_free(r->config.mem, r);
  *ptr = NULL;
  return H2_PAL_OK;
}
h2_pal_result_t
h2_gizclaw_resource_snapshot(h2_gizclaw_resource_t *r,
                             h2_gizclaw_resp_storage_t *storage,
                             h2_gizclaw_resource_snapshot_t *out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (r == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock(r);
  if (rc != H2_PAL_OK)
    return rc;
  rc = copy_snapshot(&r->snapshot, storage, out);
  unlock(r);
  return rc;
}
h2_pal_result_t h2_gizclaw_resource_close(h2_gizclaw_resource_t *r) {
  if (r == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock(r);
  if (rc != H2_PAL_OK)
    return rc;
  if (!r->snapshot.closed) {
    r->snapshot.closed = true;
    r->snapshot.stale = true;
    changed(r);
  }
  unlock(r);
  return H2_PAL_OK;
}
static h2_pal_result_t remaining(h2_gizclaw_resource_t *r, uint32_t *left) {
  h2_pal_result_t rc = lock(r);
  if (rc != H2_PAL_OK)
    return rc;
  bool closed = r->snapshot.closed;
  unlock(r);
  if (closed)
    return H2_PAL_ERR_CLOSED;
  uint64_t now = 0;
  rc = h2_pal_time_get_monotonic_ms(r->config.time, &now);
  if (rc != H2_PAL_OK)
    return rc;
  if (now >= r->deadline)
    return H2_PAL_ERR_TIMEOUT;
  *left = (uint32_t)(r->deadline - now);
  return H2_PAL_OK;
}

static h2_pal_result_t list_all(h2_gizclaw_resource_t *r,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_resource_snapshot_t *next) {
  h2_gizclaw_resp_arena_t arena;
  h2_pal_result_t rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  bool contacts = r->config.kind == H2_GIZCLAW_RESOURCE_CONTACTS;
  size_t width = contacts ? sizeof(h2_gizclaw_contact_t)
                          : sizeof(h2_gizclaw_friend_group_t);
  void *items = h2_pal_mem_alloc(&arena.allocator, r->config.max_items * width);
  rc = h2_gizclaw_resp_arena_end(&arena, H2_PAL_OK);
  if (rc != H2_PAL_OK)
    return rc;
  size_t count = 0u;
  const char *cursor = NULL;
  for (size_t page_index = 0u; page_index <= r->config.max_items;
       ++page_index) {
    uint32_t left = 0u;
    rc = remaining(r, &left);
    if (rc != H2_PAL_OK)
      return rc;
    size_t limit = r->config.max_items - count;
    if (limit == 0u)
      limit = 1u;
    if (limit > r->config.page_size)
      limit = r->config.page_size;
    h2_gizclaw_contact_page_t cp = {0};
    h2_gizclaw_friend_group_page_t gp = {0};
    rc = contacts
             ? h2_gizclaw_rpc_contact_list(r->config.service, str(cursor),
                                           limit, left, storage, &cp)
             : h2_gizclaw_rpc_friend_group_list(r->config.service, str(cursor),
                                                limit, left, storage, &gp);
    if (rc != H2_PAL_OK)
      return rc;
    size_t n = contacts ? cp.count : gp.count;
    if (n > r->config.max_items - count)
      return H2_PAL_ERR_NO_SPACE;
    for (size_t i = 0u; i < n; ++i) {
      const char *name = contacts ? cp.items[i].name : gp.items[i].name;
      if (name == NULL || name[0] == 0)
        return H2_PAL_ERR_FORMAT;
      for (size_t j = 0u; j < count; ++j) {
        const char *prior = contacts
                                ? ((h2_gizclaw_contact_t *)items)[j].name
                                : ((h2_gizclaw_friend_group_t *)items)[j].name;
        if (same(prior, name))
          return H2_PAL_ERR_FORMAT;
      }
      if (contacts)
        ((h2_gizclaw_contact_t *)items)[count] = cp.items[i];
      else
        ((h2_gizclaw_friend_group_t *)items)[count] = gp.items[i];
      ++count;
    }
    bool more = contacts ? cp.has_next : gp.has_next;
    if (!more) {
      if (contacts)
        next->data.contacts =
            (h2_gizclaw_contact_page_t){.items = items, .count = count};
      else
        next->data.groups =
            (h2_gizclaw_friend_group_page_t){.items = items, .count = count};
      next->valid = true;
      return H2_PAL_OK;
    }
    const char *following = contacts ? cp.next_cursor : gp.next_cursor;
    if (following == NULL || following[0] == 0 || same(following, cursor))
      return H2_PAL_ERR_FORMAT;
    cursor = following;
  }
  return H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t app_config_run(h2_gizclaw_resource_t *r,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_resource_snapshot_t *next) {
  h2_gizclaw_resp_arena_t arena;
  h2_pal_result_t rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK) return rc;
  h2_gizclaw_app_config_snapshot_t result = {0};
  result.items = h2_pal_mem_alloc(&arena.allocator,
      r->config.max_items * sizeof(*result.items));
  rc = h2_gizclaw_resp_arena_end(&arena, H2_PAL_OK);
  if (rc != H2_PAL_OK) return rc;
  const char *cursor = NULL;
  for (size_t page_index = 0; page_index <= r->config.max_items; ++page_index) {
    uint32_t left;
    rc = remaining(r, &left);
    if (rc != H2_PAL_OK) return rc;
    size_t limit = r->config.max_items - result.count;
    if (limit == 0) limit = 1;
    if (limit > r->config.page_size) limit = r->config.page_size;
    h2_gizclaw_app_config_page_t page = {0};
    rc = h2_gizclaw_rpc_app_config_list(r->config.service, str(cursor), limit,
                                      left, storage, &page);
    if (rc != H2_PAL_OK) return rc;
    if (page.runtime_profile_name == NULL || page.runtime_profile_name[0] == 0 ||
        page.runtime_profile_revision == NULL || page.runtime_profile_revision[0] == 0 ||
        page.count > limit || (page.count && page.keys == NULL))
      return H2_PAL_ERR_FORMAT;
    if (page_index == 0) {
      result.runtime_profile_name = page.runtime_profile_name;
      result.runtime_profile_revision = page.runtime_profile_revision;
    } else if (!same(result.runtime_profile_name, page.runtime_profile_name) ||
               !same(result.runtime_profile_revision, page.runtime_profile_revision)) {
      return H2_PAL_ERR_INVALID_STATE;
    }
    if (page.count > r->config.max_items - result.count) return H2_PAL_ERR_NO_SPACE;
    for (size_t i = 0; i < page.count; ++i) {
      const char *key = page.keys[i];
      if (key == NULL || key[0] == 0) return H2_PAL_ERR_FORMAT;
      for (size_t j = 0; j < result.count; ++j)
        if (same(result.items[j].key, key)) return H2_PAL_ERR_FORMAT;
      rc = remaining(r, &left);
      if (rc != H2_PAL_OK) return rc;
      h2_gizclaw_app_config_value_t value = {0};
      rc = h2_gizclaw_rpc_app_config_get(r->config.service, str(key), left, storage, &value);
      if (rc != H2_PAL_OK) return rc;
      if (!same(result.runtime_profile_name, value.runtime_profile_name) ||
          !same(result.runtime_profile_revision, value.runtime_profile_revision))
        return H2_PAL_ERR_INVALID_STATE;
      result.items[result.count++] = (h2_gizclaw_app_config_entry_t){page.keys[i], value.value};
    }
    if (!page.has_next) {
      next->data.app_config = result;
      next->valid = true;
      return H2_PAL_OK;
    }
    if (page.count == 0 || page.next_cursor == NULL || page.next_cursor[0] == 0 ||
        same(cursor, page.next_cursor)) return H2_PAL_ERR_FORMAT;
    cursor = page.next_cursor;
  }
  return H2_PAL_ERR_FORMAT;
}

static bool command_valid(h2_gizclaw_resource_t *r,
                          const h2_gizclaw_resource_command_t *c) {
  if (c == NULL)
    return false;
  if (c->operation == H2_GIZCLAW_RESOURCE_REFRESH)
    return true;
  switch (r->config.kind) {
  case H2_GIZCLAW_RESOURCE_CONTACTS:
    if (c->operation < H2_GIZCLAW_RESOURCE_CONTACT_CREATE ||
        c->operation > H2_GIZCLAW_RESOURCE_CONTACT_DELETE || c->name == NULL ||
        c->name[0] == 0 || strlen(c->name) > H2_GIZCLAW_CONTACT_NAME_MAX_BYTES)
      return false;
    return c->operation == H2_GIZCLAW_RESOURCE_CONTACT_DELETE ||
           (c->display_name != NULL && c->phone_number != NULL &&
            c->display_name[0] != 0 && c->phone_number[0] != 0 &&
            strlen(c->display_name) <=
                H2_GIZCLAW_CONTACT_DISPLAY_NAME_MAX_BYTES &&
            strlen(c->phone_number) <=
                H2_GIZCLAW_CONTACT_PHONE_NUMBER_MAX_BYTES);
  case H2_GIZCLAW_RESOURCE_PROFILE:
    return c->text != NULL && c->text[0] != 0 &&
           ((c->operation == H2_GIZCLAW_RESOURCE_PROFILE_NAME &&
             strlen(c->text) <= H2_GIZCLAW_PROFILE_NAME_MAX_BYTES) ||
            (c->operation == H2_GIZCLAW_RESOURCE_PROFILE_EMOJI &&
             strlen(c->text) <= H2_GIZCLAW_PROFILE_EMOJI_MAX_BYTES));
  default:
    return false;
  }
}

static h2_pal_result_t contact_mutate(h2_gizclaw_resource_t *r,
                                      const h2_gizclaw_resource_command_t *c,
                                      h2_gizclaw_resp_storage_t *storage) {
  uint32_t left = 0;
  h2_pal_result_t rc = remaining(r, &left);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_contact_t result = {0};
  if (c->operation == H2_GIZCLAW_RESOURCE_CONTACT_CREATE) {
    rc = h2_gizclaw_rpc_contact_get(r->config.service, str(c->name), left,
                                    storage, &result);
    if (rc == H2_PAL_ERR_NOT_FOUND) {
      rc = remaining(r, &left);
      if (rc != H2_PAL_OK)
        return rc;
      rc = h2_gizclaw_rpc_contact_create(
          r->config.service, str(c->name), str(c->display_name),
          str(c->phone_number), left, storage, &result);
      if (rc != H2_PAL_OK) {
        h2_pal_result_t create_rc = rc;
        rc = remaining(r, &left);
        if (rc != H2_PAL_OK)
          return rc;
        rc = h2_gizclaw_rpc_contact_get(r->config.service, str(c->name), left,
                                        storage, &result);
        if (rc != H2_PAL_OK)
          return create_rc;
      }
    }
  } else if (c->operation == H2_GIZCLAW_RESOURCE_CONTACT_UPDATE) {
    rc = h2_gizclaw_rpc_contact_put(r->config.service, str(c->name),
                                    str(c->display_name), str(c->phone_number),
                                    left, storage, &result);
  } else {
    rc = h2_gizclaw_rpc_contact_delete(r->config.service, str(c->name), left,
                                       storage, &result);
    return rc == H2_PAL_ERR_NOT_FOUND ? H2_PAL_OK : rc;
  }
  if (rc == H2_PAL_OK && (!same(result.name, c->name) ||
                          !same(result.display_name, c->display_name) ||
                          !same(result.phone_number, c->phone_number)))
    return H2_PAL_ERR_INVALID_STATE;
  return rc;
}

static h2_pal_result_t profile_run(h2_gizclaw_resource_t *r,
                                   const h2_gizclaw_resource_command_t *c,
                                   h2_gizclaw_resource_snapshot_t *next) {
  uint32_t left = 0;
  h2_pal_result_t rc = remaining(r, &left);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_profile_t result = {0};
  if (c->operation == H2_GIZCLAW_RESOURCE_PROFILE_NAME)
    rc = h2_gizclaw_rpc_profile_put_name(r->config.service, str(c->text), left,
                                         &result);
  else if (c->operation == H2_GIZCLAW_RESOURCE_PROFILE_EMOJI)
    rc = h2_gizclaw_rpc_profile_put_emoji(r->config.service, str(c->text), left,
                                          &result);
  if (rc != H2_PAL_OK)
    return rc;
  rc = remaining(r, &left);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_gizclaw_rpc_profile_get(r->config.service, left, &result);
  if (rc != H2_PAL_OK)
    return rc;
  if ((c->operation == H2_GIZCLAW_RESOURCE_PROFILE_NAME &&
       (!result.has_name || !same(result.name, c->text))) ||
      (c->operation == H2_GIZCLAW_RESOURCE_PROFILE_EMOJI &&
       (!result.has_emoji || !same(result.emoji, c->text))))
    return H2_PAL_ERR_INVALID_STATE;
  next->data.profile = result;
  next->valid = true;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_resource_execute(h2_gizclaw_resource_t *r,
                            const h2_gizclaw_resource_command_t *c,
                            uint32_t timeout_ms) {
  if (r == NULL || !command_valid(r, c) || timeout_ms == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  uint64_t now = 0u;
  h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(r->config.time, &now);
  if (rc != H2_PAL_OK)
    return rc;
  if (UINT64_MAX - now < timeout_ms)
    return H2_PAL_ERR_INVALID_ARG;
  rc = lock(r);
  if (rc != H2_PAL_OK)
    return rc;
  if (r->snapshot.closed || r->snapshot.busy) {
    rc = r->snapshot.closed ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_BUSY;
    unlock(r);
    return rc;
  }
  r->snapshot.busy = true;
  r->snapshot.stale = true;
  r->deadline = now + timeout_ms;
  changed(r);
  unlock(r);
  uint8_t *data = h2_pal_mem_alloc(r->config.mem, r->config.storage_bytes);
  h2_gizclaw_resp_storage_t storage = {data, r->config.storage_bytes, 0u};
  h2_gizclaw_resource_snapshot_t next = {0};
  if (data == NULL)
    rc = H2_PAL_ERR_NO_MEMORY;
  else
    rc = h2_gizclaw_resource_snapshot(r, &storage, &next);
  uint64_t old_data_revision = next.data_revision;
  bool commit = false;
  if (rc == H2_PAL_OK) {
    if (r->config.kind == H2_GIZCLAW_RESOURCE_PROFILE) {
      rc = profile_run(r, c, &next);
    } else if (r->config.kind == H2_GIZCLAW_RESOURCE_FIRMWARE) {
      uint32_t left = 0;
      rc = remaining(r, &left);
      if (rc == H2_PAL_OK)
        rc = h2_gizclaw_rpc_firmware_get(r->config.service,
                                        r->config.firmware_channel, left,
                                        &next.data.firmware);
      if (rc == H2_PAL_OK &&
          next.data.firmware.channel != r->config.firmware_channel)
        rc = H2_PAL_ERR_FORMAT;
      if (rc == H2_PAL_OK)
        next.valid = true;
    } else if (r->config.kind == H2_GIZCLAW_RESOURCE_APP_CONFIG) {
      rc = app_config_run(r, &storage, &next);
    } else {
      if (c->operation != H2_GIZCLAW_RESOURCE_REFRESH)
        rc = contact_mutate(r, c, &storage);
      if (rc == H2_PAL_OK)
        rc = list_all(r, &storage, &next);
    }
    if (rc == H2_PAL_OK)
      commit = true;
  }
  uint32_t left = 0;
  h2_pal_result_t deadline_rc = remaining(r, &left);
  if (deadline_rc != H2_PAL_OK) {
    rc = deadline_rc;
    commit = false;
  }
  h2_pal_result_t lock_rc = lock(r);
  if (lock_rc != H2_PAL_OK) {
    h2_pal_mem_free(r->config.mem, data);
    return lock_rc;
  }
  if (r->snapshot.closed) {
    rc = H2_PAL_ERR_CLOSED;
    commit = false;
  }
  if (commit && old_data_revision == UINT64_MAX) {
    rc = H2_PAL_ERR_INVALID_STATE;
    commit = false;
  }
  if (commit) {
    next.revision = r->snapshot.revision;
    next.data_revision = old_data_revision + 1u;
    h2_pal_mem_free(r->config.mem, r->data);
    r->data = data;
    data = NULL;
    r->snapshot = next;
  }
  r->snapshot.busy = false;
  r->snapshot.stale = rc != H2_PAL_OK || r->snapshot.closed;
  r->snapshot.last_error = rc;
  changed(r);
  unlock(r);
  h2_pal_mem_free(r->config.mem, data);
  return rc;
}
