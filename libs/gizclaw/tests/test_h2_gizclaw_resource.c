#include "h2_desktop_platform.h"
#include "h2_gizclaw_resource.h"
#include "h2_gizclaw_response_internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

/* Only typed RPCs are replaced. All Resource state and allocation is real. */
static h2_gizclaw_resource_t *resource;
static unsigned mode, calls, gets, creates;
static uint64_t now;
static bool exists;
static h2_pal_result_t balance_rc, transactions_rc;
static const h2_gizclaw_resource_command_t refresh = {
    .operation = H2_GIZCLAW_RESOURCE_REFRESH};
static h2_pal_result_t monotonic(void *user, uint64_t *out) {
  (void)user;
  *out = now;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t tv = {.get_monotonic_ms = monotonic};
static const h2_pal_time_api_t time_api = {.vtable = &tv};
static char *copy(h2_gizclaw_resp_arena_t *a, const char *s) {
  char *p = h2_pal_mem_alloc(&a->allocator, strlen(s) + 1u);
  assert(p != NULL);
  strcpy(p, s);
  return p;
}
static void begin_rpc(h2_gizclaw_service_t *service, uint32_t timeout) {
  assert(service != NULL && timeout > 0);
  ++calls;
}
static void in_flight(void) {
  if (mode == 4)
    now += 100;
  if (mode == 5) {
    assert(h2_gizclaw_resource_execute(resource, &refresh, 100) ==
           H2_PAL_ERR_BUSY);
    h2_gizclaw_resource_t *alias = resource;
    assert(h2_gizclaw_resource_destroy(&alias) == H2_PAL_ERR_BUSY &&
           alias == resource);
    assert(h2_gizclaw_resource_close(resource) == H2_PAL_OK);
  }
}
static h2_pal_result_t contact(h2_gizclaw_resp_storage_t *storage,
                               h2_gizclaw_contact_t *out, const char *name) {
  h2_gizclaw_resp_arena_t a;
  assert(h2_gizclaw_resp_arena_begin(storage, &a) == H2_PAL_OK);
  *out = (h2_gizclaw_contact_t){.name = copy(&a, name),
                                .display_name = copy(&a, "Alice"),
                                .phone_number = copy(&a, "123")};
  return h2_gizclaw_resp_arena_end(&a, H2_PAL_OK);
}
h2_pal_result_t h2_gizclaw_rpc_contact_list(h2_gizclaw_service_t *s,
                                            h2_gizclaw_str_t cursor,
                                            size_t limit, uint32_t timeout,
                                            h2_gizclaw_resp_storage_t *storage,
                                            h2_gizclaw_contact_page_t *out) {
  begin_rpc(s, timeout);
  assert(limit > 0);
  in_flight();
  h2_gizclaw_resp_arena_t a;
  assert(h2_gizclaw_resp_arena_begin(storage, &a) == H2_PAL_OK);
  *out = (h2_gizclaw_contact_page_t){0};
  out->items = h2_pal_mem_alloc(&a.allocator, sizeof(*out->items));
  assert(out->items != NULL);
  out->has_next = cursor.len == 0 || mode == 3;
  out->next_cursor = out->has_next ? copy(&a, "next") : NULL;
  assert(h2_gizclaw_resp_arena_end(&a, H2_PAL_OK) == H2_PAL_OK);
  out->count = 1;
  return contact(storage, out->items, cursor.len == 0 || mode == 1 ? "a" : "b");
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_list(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_page_t *out) {
  begin_rpc(s, timeout);
  assert(limit > 0);
  h2_gizclaw_resp_arena_t a;
  assert(h2_gizclaw_resp_arena_begin(storage, &a) == H2_PAL_OK);
  *out = (h2_gizclaw_friend_group_page_t){0};
  out->items = h2_pal_mem_alloc(&a.allocator, sizeof(*out->items));
  assert(out->items);
  *out->items = (h2_gizclaw_friend_group_t){
      .name = copy(&a, cursor.len && mode != 1 ? "b" : "a")};
  out->count = 1;
  out->has_next = cursor.len == 0;
  out->next_cursor = out->has_next ? copy(&a, "next") : NULL;
  return h2_gizclaw_resp_arena_end(&a, H2_PAL_OK);
}
h2_pal_result_t h2_gizclaw_rpc_contact_get(h2_gizclaw_service_t *s,
                                           h2_gizclaw_str_t name,
                                           uint32_t timeout,
                                           h2_gizclaw_resp_storage_t *storage,
                                           h2_gizclaw_contact_t *out) {
  begin_rpc(s, timeout);
  ++gets;
  if (!exists)
    return H2_PAL_ERR_NOT_FOUND;
  return contact(storage, out, mode == 6 ? "wrong" : name.data);
}
h2_pal_result_t h2_gizclaw_rpc_contact_create(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t name, h2_gizclaw_str_t display,
    h2_gizclaw_str_t phone, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_contact_t *out) {
  begin_rpc(s, timeout);
  (void)name;
  (void)display;
  (void)phone;
  (void)storage;
  (void)out;
  ++creates;
  exists = true;
  return H2_PAL_ERR_TIMEOUT; /* applied, response lost */
}
h2_pal_result_t
h2_gizclaw_rpc_contact_put(h2_gizclaw_service_t *s, h2_gizclaw_str_t name,
                           h2_gizclaw_str_t display, h2_gizclaw_str_t phone,
                           uint32_t timeout, h2_gizclaw_resp_storage_t *storage,
                           h2_gizclaw_contact_t *out) {
  (void)display;
  (void)phone;
  begin_rpc(s, timeout);
  return contact(storage, out, name.data);
}
h2_pal_result_t h2_gizclaw_rpc_contact_delete(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t name, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_contact_t *out) {
  (void)name;
  (void)storage;
  (void)out;
  begin_rpc(s, timeout);
  return H2_PAL_ERR_NOT_FOUND;
}
h2_pal_result_t h2_gizclaw_rpc_profile_get(h2_gizclaw_service_t *s,
                                           uint32_t timeout,
                                           h2_gizclaw_profile_t *out) {
  begin_rpc(s, timeout);
  *out = (h2_gizclaw_profile_t){.has_name = true};
  strcpy(out->name, "Alice");
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_profile_put_name(h2_gizclaw_service_t *s,
                                                h2_gizclaw_str_t name,
                                                uint32_t timeout,
                                                h2_gizclaw_profile_t *out) {
  (void)name;
  return h2_gizclaw_rpc_profile_get(s, timeout, out);
}
h2_pal_result_t h2_gizclaw_rpc_profile_put_emoji(h2_gizclaw_service_t *s,
                                                 h2_gizclaw_str_t emoji,
                                                 uint32_t timeout,
                                                 h2_gizclaw_profile_t *out) {
  (void)emoji;
  return h2_gizclaw_rpc_profile_get(s, timeout, out);
}
h2_pal_result_t h2_gizclaw_rpc_point_get(h2_gizclaw_service_t *s,
                                         uint32_t timeout,
                                         h2_gizclaw_resp_storage_t *storage,
                                         h2_gizclaw_points_account_t *out) {
  (void)storage;
  begin_rpc(s, timeout);
  memset(out, 0, sizeof(*out));
  return balance_rc;
}
h2_pal_result_t h2_gizclaw_rpc_point_transaction_list(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_points_transaction_page_t *out) {
  begin_rpc(s, timeout);
  assert(limit > 0);
  if (transactions_rc != H2_PAL_OK)
    return transactions_rc;
  h2_gizclaw_resp_arena_t a;
  assert(h2_gizclaw_resp_arena_begin(storage, &a) == H2_PAL_OK);
  *out = (h2_gizclaw_points_transaction_page_t){0};
  out->items = h2_pal_mem_alloc(&a.allocator, sizeof(*out->items));
  assert(out->items);
  *out->items = (h2_gizclaw_points_transaction_t){
      .id = {copy(&a, cursor.len ? "b" : "a"), 1}};
  out->count = 1;
  out->has_next = cursor.len == 0;
  if (out->has_next)
    out->next_cursor = (h2_gizclaw_owned_text_t){copy(&a, "next"), 4};
  return h2_gizclaw_resp_arena_end(&a, H2_PAL_OK);
}
static void setup(h2_gizclaw_resource_kind_t kind, size_t max_items) {
  mode = calls = gets = creates = 0;
  now = 0;
  exists = false;
  balance_rc = transactions_rc = H2_PAL_OK;
  h2_gizclaw_resource_config_t c = {.kind = kind,
                                    .service = (h2_gizclaw_service_t *)&calls,
                                    .mem = h2_desktop_platform_default_allocator(),
                                    .sync = h2_desktop_platform_sync_api(),
                                    .time = &time_api,
                                    .max_items = max_items,
                                    .page_size = 1,
                                    .storage_bytes = 16384};
  assert(h2_gizclaw_resource_create(&c, &resource) == H2_PAL_OK);
}
static h2_gizclaw_resource_snapshot_t snapshot(void) {
  static uint8_t data[16384];
  h2_gizclaw_resp_storage_t storage = {data, sizeof(data), 0};
  h2_gizclaw_resource_snapshot_t out;
  assert(h2_gizclaw_resource_snapshot(resource, &storage, &out) == H2_PAL_OK);
  return out;
}
int main(void) {
  for (unsigned scenario = 0; scenario < 7; ++scenario) {
    setup(H2_GIZCLAW_RESOURCE_CONTACTS, scenario == 2 ? 1 : 4);
    if (scenario != 2)
      assert(h2_gizclaw_resource_execute(resource, &refresh, 100) == H2_PAL_OK);
    h2_gizclaw_resource_snapshot_t old = snapshot();
    mode = scenario;
    h2_pal_result_t rc = h2_gizclaw_resource_execute(resource, &refresh, 100);
    assert(rc == (scenario == 1 || scenario == 3 ? H2_PAL_ERR_FORMAT
                  : scenario == 2                ? H2_PAL_ERR_NO_SPACE
                  : scenario == 4                ? H2_PAL_ERR_TIMEOUT
                  : scenario == 5                ? H2_PAL_ERR_CLOSED
                                                 : H2_PAL_OK));
    h2_gizclaw_resource_snapshot_t after = snapshot();
    if (rc != H2_PAL_OK)
      assert(after.stale && after.data_revision == old.data_revision);
    if (scenario == 5)
      assert(after.closed && !after.busy);
    assert(h2_gizclaw_resource_destroy(&resource) == H2_PAL_OK);
  }
  setup(H2_GIZCLAW_RESOURCE_CONTACTS, 4);
  const h2_gizclaw_resource_command_t create = {
      .operation = H2_GIZCLAW_RESOURCE_CONTACT_CREATE,
      .name = "a",
      .display_name = "Alice",
      .phone_number = "123"};
  assert(h2_gizclaw_resource_execute(resource, &create, 100) == H2_PAL_OK);
  assert(creates == 1 && gets == 2);
  h2_gizclaw_resource_snapshot_t old = snapshot();
  strcpy(old.data.contacts.items[0].name, "x");
  assert(strcmp(snapshot().data.contacts.items[0].name, "a") == 0);
  uint8_t tiny[1];
  h2_gizclaw_resp_storage_t storage = {tiny, 1, 0};
  assert(h2_gizclaw_resource_snapshot(resource, &storage, &old) ==
         H2_PAL_ERR_NO_SPACE);
  assert(storage.used == 0 && !old.valid);
  mode = 6;
  assert(h2_gizclaw_resource_execute(resource, &create, 100) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(creates == 1);
  assert(h2_gizclaw_resource_destroy(&resource) == H2_PAL_OK);
  setup(H2_GIZCLAW_RESOURCE_GROUPS, 4);
  assert(h2_gizclaw_resource_execute(resource, &refresh, 100) == H2_PAL_OK);
  assert(snapshot().data.groups.count == 2);
  mode = 1;
  assert(h2_gizclaw_resource_execute(resource, &refresh, 100) ==
         H2_PAL_ERR_FORMAT);
  assert(h2_gizclaw_resource_destroy(&resource) == H2_PAL_OK);
  setup(H2_GIZCLAW_RESOURCE_PROFILE, 4);
  assert(h2_gizclaw_resource_execute(resource, &refresh, 100) == H2_PAL_OK);
  assert(strcmp(snapshot().data.profile.name, "Alice") == 0);
  assert(h2_gizclaw_resource_destroy(&resource) == H2_PAL_OK);
  setup(H2_GIZCLAW_RESOURCE_POINTS, 4);
  balance_rc = H2_PAL_ERR_IO;
  assert(h2_gizclaw_resource_execute(resource, &refresh, 100) == H2_PAL_OK);
  assert(snapshot().valid && !snapshot().balance_valid &&
         snapshot().balance_result == H2_PAL_ERR_IO);
  const h2_gizclaw_resource_command_t more = {.operation = H2_GIZCLAW_RESOURCE_LOAD_MORE};
  assert(h2_gizclaw_resource_execute(resource, &more, 100) == H2_PAL_OK);
  assert(snapshot().data.points.transactions.count == 2);
  balance_rc = H2_PAL_OK;
  transactions_rc = H2_PAL_ERR_IO;
  assert(h2_gizclaw_resource_execute(resource, &refresh, 100) == H2_PAL_ERR_IO);
  assert(snapshot().balance_valid && snapshot().stale &&
         snapshot().data.points.transactions.count == 2);
  assert(h2_gizclaw_resource_execute(resource, &more, 100) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_resource_destroy(&resource) == H2_PAL_OK);
  return 0;
}
