#include "h2_gizclaw_e2e_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_mutex_storage;
static uint64_t s_now = 100u;
static unsigned s_starts, s_stops, s_registers, s_deletes, s_polls;
static int s_start_rc, s_register_rc, s_delete_rc, s_stop_rc, s_deinit_rc;
static const char *s_profile = "runtime-profile-from-server";
static bool s_unterminated_profile;
static bool s_null_service;
static unsigned s_live_services;
static unsigned s_poll_fault;
static bool s_user_stop;

static bool user_stop(void *user) {
  (void)user;
  return s_user_stop;
}

/* Job-task double: the job body runs from the App-side poll (or from the
 * Service stop that cancels it) instead of on a real task, so the sync-call
 * lifecycle is deterministic. */
static int s_task_storage;
static h2_pal_task_entry_t s_job_entry;
static void *s_job_ctx;
static unsigned s_job_after_polls, s_job_polls, s_task_starts, s_task_joins,
    s_join_failures;
static int s_task_start_rc;

static void run_pending_job(void) {
  if (s_job_entry == NULL)
    return;
  const h2_pal_task_entry_t entry = s_job_entry;
  s_job_entry = NULL;
  entry(s_job_ctx);
}

static int test_task_start(void *user, const h2_pal_task_options_t *options,
                           h2_pal_task_entry_t entry, void *ctx,
                           h2_pal_task_t **out_task) {
  (void)user;
  assert(options != NULL && strcmp(options->name, "gizclaw/e2e/job") == 0 &&
         options->min_stack_size == 32768u);
  ++s_task_starts;
  *out_task = NULL;
  if (s_task_start_rc != H2_PAL_OK)
    return s_task_start_rc;
  s_job_entry = entry;
  s_job_ctx = ctx;
  s_job_polls = 0u;
  *out_task = (h2_pal_task_t *)&s_task_storage;
  return H2_PAL_OK;
}

static int test_task_join(void *user, h2_pal_task_t *task) {
  (void)user;
  assert(task == (h2_pal_task_t *)&s_task_storage);
  ++s_task_joins;
  if (s_join_failures != 0u) {
    --s_join_failures;
    return H2_PAL_ERR_BUSY;
  }
  return H2_PAL_OK;
}

/* Fixture boundary doubles: no network or registration side effect. The real
 * Service concurrency/lifetime paths are exercised by the library tests. */
struct h2_gizclaw_service {
  h2_gizclaw_service_config_t config;
  bool started;
  bool stopped;
};

static unsigned s_case_cleanups;
static int s_case_cleanup_rc;
static bool s_case_expect_stopped;
static int cleanup_case(h2_gizclaw_e2e_fixture_t *fixture) {
  assert(fixture->case_state == &s_case_cleanups);
  assert(fixture->actors[0].service != NULL);
  assert(fixture->actors[0].service->stopped == s_case_expect_stopped);
  assert(fixture->registration_token != NULL &&
         fixture->actors[0].private_key[0]);
  ++s_case_cleanups;
  if (s_case_cleanup_rc != H2_PAL_OK)
    return s_case_cleanup_rc;
  fixture->case_state = NULL;
  fixture->case_cleanup = NULL;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_service_init(const h2_gizclaw_service_config_t *config,
                        h2_gizclaw_service_t **out_service) {
  assert(config->task != NULL && config->queue != NULL && config->sync != NULL);
  assert(config->client_poll_timeout_ms == 1);
  assert(config->operation_capacity >= 3u);
  if (s_null_service) {
    *out_service = NULL;
    return H2_PAL_OK;
  }
  *out_service = calloc(1, sizeof(**out_service));
  assert(*out_service != NULL);
  (*out_service)->config = *config;
  ++s_live_services;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_service_start(h2_gizclaw_service_t *service) {
  ++s_starts;
  service->started = s_start_rc == H2_PAL_OK;
  return s_start_rc;
}

h2_pal_result_t h2_gizclaw_service_poll(h2_gizclaw_service_t *service,
                                        size_t maximum, size_t *out_count) {
  assert(service != NULL && maximum > 0u);
  ++s_polls;
  *out_count = s_poll_fault == 1u   ? maximum + 1u
               : s_poll_fault == 2u ? maximum
                                    : 0u;
  /* The job's request completes only through App-side dispatch. */
  if (s_job_entry != NULL && s_job_after_polls != 0u &&
      ++s_job_polls >= s_job_after_polls)
    run_pending_job();
  return s_poll_fault == 3u ? H2_PAL_ERR_IO : H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_service_stop(h2_gizclaw_service_t *service) {
  ++s_stops;
  service->stopped = s_stop_rc == H2_PAL_OK;
  /* Stop cancels the job's request, so its wait returns. */
  run_pending_job();
  return s_stop_rc;
}

h2_pal_result_t h2_gizclaw_service_deinit(h2_gizclaw_service_t *service) {
  assert(service->stopped);
  assert(service->config.client_config->private_key.len != 0u);
  if (s_deinit_rc == H2_PAL_OK) {
    --s_live_services;
    free(service);
  }
  return s_deinit_rc;
}

h2_pal_result_t
h2_gizclaw_rpc_register(h2_gizclaw_service_t *service, const char *token,
                        uint32_t timeout_ms,
                        h2_gizclaw_registration_result_t *out_result) {
  assert(service->started && !service->stopped);
  assert(strcmp(token, "borrowed-token") == 0 && timeout_ms > 0u);
  ++s_registers;
  if (s_unterminated_profile)
    memset(out_result->runtime_profile_name, 'x',
           sizeof(out_result->runtime_profile_name));
  else
    strcpy(out_result->runtime_profile_name, s_profile);
  return s_register_rc;
}

h2_pal_result_t h2_gizclaw_rpc_peer_delete(h2_gizclaw_service_t *service,
                                           uint32_t timeout_ms) {
  assert(service->started && !service->stopped && timeout_ms > 0u);
  assert(!service->config.client_config->cancel_requested(
      service->config.client_config->cancel_user));
  ++s_deletes;
  return s_delete_rc;
}

/* Fail closed unless the test explicitly grants the exact business scope. */
enum { CONTACT = 1u, FRIEND = 2u, GROUP = 4u, PET = 8u, WORKSPACE = 16u };
static unsigned s_business_allowed, s_business_failed, s_business_seen;
static h2_gizclaw_e2e_fixture_t *s_business_fixture;
static bool s_lookup_friend;
static int s_lookup_rc;
enum {
  LOOKUP_NORMAL,
  LOOKUP_SECOND_PAGE,
  LOOKUP_ABSENT,
  LOOKUP_REPEAT,
  LOOKUP_CYCLE,
  LOOKUP_DUPLICATE,
  LOOKUP_BAD_ID,
  LOOKUP_NULL_ITEMS,
  LOOKUP_TOO_MANY,
  LOOKUP_NULL_KEY,
  LOOKUP_UNTERMINATED,
  LOOKUP_UNOWNED_ITEMS,
  LOOKUP_UNALIGNED_ITEMS,
  LOOKUP_BAD_CURSOR,
  LOOKUP_LONG_CURSOR,
  LOOKUP_SECOND_ERROR,
  LOOKUP_LATE_TIMEOUT
};
static unsigned s_lookup_mode, s_lookup_calls, s_friend_delete_mode;
static bool s_owner_group;
static int s_group_invite_rc;
static unsigned s_workspace_mode, s_workspace_role = H2_GIZCLAW_E2E_FRIEND;
static unsigned s_workspace_deletes, s_workspace_gets;
static bool s_member_cleanup;
static unsigned s_member_mode, s_member_lists, s_member_deletes;
#define BUSINESS_DELETE(name, type, bit, field, role)                          \
  h2_pal_result_t h2_gizclaw_rpc_##name(                                       \
      h2_gizclaw_service_t *service, h2_gizclaw_str_t id, uint32_t timeout,    \
      h2_gizclaw_resp_storage_t *storage, type *out) {                         \
    assert((s_business_allowed & bit) != 0u && s_business_fixture != NULL);    \
    assert(service == s_business_fixture->actors[role].service);               \
    assert(id.len == strlen(s_business_fixture->field) && id.len > 0u);        \
    assert(memcmp(id.data, s_business_fixture->field, id.len) == 0);           \
    assert(timeout == 15000u && storage != NULL && out != NULL);               \
    s_business_seen |= bit;                                                    \
    if (bit == GROUP && s_member_cleanup)                                      \
      assert(!s_business_fixture->friend_group_member_joined);                 \
    return (s_business_failed & bit) ? H2_PAL_ERR_IO : H2_PAL_OK;              \
  }
BUSINESS_DELETE(contact_delete, h2_gizclaw_contact_t, CONTACT, contact_name,
                H2_GIZCLAW_E2E_FRIEND)
BUSINESS_DELETE(friend_group_delete, h2_gizclaw_friend_group_t, GROUP,
                friend_group_name,
                (s_owner_group ? H2_GIZCLAW_E2E_OWNER : H2_GIZCLAW_E2E_FRIEND))
#undef BUSINESS_DELETE

static char *cleanup_save(h2_gizclaw_resp_storage_t *storage,
                          const char *text) {
  size_t size = strlen(text) + 1u;
  assert(storage->used + size <= storage->capacity);
  char *out = (char *)storage->data + storage->used;
  memcpy(out, text, size);
  storage->used += size;
  return out;
}

static void workspace_call(h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
                           uint32_t timeout,
                           h2_gizclaw_resp_storage_t *storage) {
  assert(s_business_fixture && (s_business_allowed & WORKSPACE));
  assert(service == s_business_fixture->actors[s_workspace_role].service);
  assert(name.len == strlen(s_business_fixture->workspace_name) &&
         name.len > 0u);
  assert(memcmp(name.data, s_business_fixture->workspace_name, name.len) == 0);
  assert(timeout == 15000u && storage->used == 0u);
  s_business_seen |= WORKSPACE;
}

static unsigned s_pet_mode, s_pet_role = H2_GIZCLAW_E2E_FRIEND;
static unsigned s_pet_deletes, s_pet_gets;
static void pet_call(h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
                     uint32_t timeout, h2_gizclaw_resp_storage_t *storage) {
  assert(s_business_fixture && (s_business_allowed & PET));
  assert(service == s_business_fixture->actors[s_pet_role].service);
  assert(name.len == strlen(s_business_fixture->pet_name) && name.len > 0u);
  assert(!memcmp(name.data, s_business_fixture->pet_name, name.len));
  assert(timeout == 15000u && !storage->used);
  s_business_seen |= PET;
}
h2_pal_result_t h2_gizclaw_rpc_pet_delete(h2_gizclaw_service_t *service,
                                          h2_gizclaw_str_t name,
                                          uint32_t timeout,
                                          h2_gizclaw_resp_storage_t *storage,
                                          h2_gizclaw_pet_t *out) {
  pet_call(service, name, timeout, storage);
  ++s_pet_deletes;
  if ((s_business_failed & PET) || s_pet_mode == 7u)
    return H2_PAL_ERR_IO;
  if (s_pet_mode == 6u)
    return H2_PAL_ERR_NOT_FOUND;
  *out = (h2_gizclaw_pet_t){
      .name = cleanup_save(storage, s_business_fixture->pet_name)};
  if (s_pet_mode == 1u)
    out->name = cleanup_save(storage, "wrong");
  if (s_pet_mode == 2u)
    out->name = NULL;
  if (s_pet_mode == 3u)
    out->name = "unowned";
  if (s_pet_mode == 4u)
    out->name[0] = '\0';
  if (s_pet_mode == 5u)
    memset(storage->data, 'x', storage->used);
  if (s_pet_mode == 8u)
    storage->used = storage->capacity + 1u;
  if (s_pet_mode == 10u)
    s_now += 45000u;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_pet_get(h2_gizclaw_service_t *service,
                                       h2_gizclaw_str_t name, uint32_t timeout,
                                       h2_gizclaw_resp_storage_t *storage,
                                       h2_gizclaw_pet_t *out) {
  pet_call(service, name, timeout, storage);
  ++s_pet_gets;
  if (s_pet_mode == 9u)
    return H2_PAL_ERR_IO;
  if ((s_pet_mode >= 11u && s_pet_mode <= 15u) ||
      (s_pet_mode == 18u && s_pet_gets < 3u)) {
    *out = (h2_gizclaw_pet_t){
        .name = cleanup_save(storage, s_business_fixture->pet_name)};
    if (s_pet_mode == 11u)
      out->name = cleanup_save(storage, "wrong");
    if (s_pet_mode == 13u)
      out->name = "unowned";
    if (s_pet_mode == 14u)
      storage->used = storage->capacity + 1u;
    if (s_pet_mode == 15u)
      memset(storage->data, 'x', storage->used);
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

h2_pal_result_t h2_gizclaw_rpc_workspace_delete(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out) {
  workspace_call(service, name, timeout, storage);
  ++s_workspace_deletes;
  if (s_business_failed & WORKSPACE)
    return H2_PAL_ERR_IO;
  *out = (h2_gizclaw_workspace_t){
      .name = cleanup_save(storage, s_business_fixture->workspace_name)};
  if (s_workspace_mode == 1u)
    out->name = cleanup_save(storage, "wrong-workspace");
  if (s_workspace_mode == 2u)
    out->name = NULL;
  if (s_workspace_mode == 3u)
    out->name = "unowned";
  if (s_workspace_mode == 4u)
    out->name[0] = '\0';
  if (s_workspace_mode == 5u)
    memset(storage->data, 'x', storage->used);
  if (s_workspace_mode == 6u)
    out->system = true;
  if (s_workspace_mode == 7u)
    storage->used = storage->capacity + 1u;
  if (s_workspace_mode == 8u)
    return H2_PAL_ERR_NOT_FOUND;
  if (s_workspace_mode == 9u)
    return H2_PAL_ERR_IO;
  if (s_workspace_mode == 10u)
    s_now += 45000u;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_rpc_workspace_get(h2_gizclaw_service_t *service,
                             h2_gizclaw_str_t name, uint32_t timeout,
                             h2_gizclaw_resp_storage_t *storage,
                             h2_gizclaw_workspace_get_result_t *out) {
  workspace_call(service, name, timeout, storage);
  ++s_workspace_gets;
  if (s_workspace_mode == 11u)
    return H2_PAL_ERR_IO;
  if (s_workspace_mode >= 12u && s_workspace_mode <= 15u) {
    *out = (h2_gizclaw_workspace_get_result_t){0};
    out->workspace.name =
        cleanup_save(storage, s_business_fixture->workspace_name);
    if (s_workspace_mode == 13u)
      out->workspace.name = cleanup_save(storage, "wrong-workspace");
    if (s_workspace_mode == 14u)
      out->workspace.name = "unowned";
    if (s_workspace_mode == 15u)
      storage->used = storage->capacity + 1u;
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

h2_pal_result_t h2_gizclaw_rpc_friend_delete(h2_gizclaw_service_t *service,
                                             h2_gizclaw_str_t id,
                                             uint32_t timeout,
                                             h2_gizclaw_resp_storage_t *storage,
                                             h2_gizclaw_friend_t *out) {
  assert((s_business_allowed & FRIEND) && s_business_fixture != NULL);
  assert(service == s_business_fixture->actors[0].service && timeout == 15000u);
  assert(id.len == strlen(s_business_fixture->friend_id) && id.len > 0u);
  assert(memcmp(id.data, s_business_fixture->friend_id, id.len) == 0);
  assert(storage->used == 0u);
  s_business_seen |= FRIEND;
  if (s_business_failed & FRIEND)
    return H2_PAL_ERR_IO;
  *out = (h2_gizclaw_friend_t){
      .id = cleanup_save(storage, s_business_fixture->friend_id),
      .peer_public_key =
          cleanup_save(storage, s_business_fixture->actors[1].public_key)};
  switch (s_friend_delete_mode) {
  case 1:
    out->id = cleanup_save(storage, "wrong-id");
    break;
  case 2:
    out->peer_public_key = NULL;
    break;
  case 3:
    out->id = "not-in-response-storage";
    break;
  case 4:
    memset(storage->data, 'x', storage->used);
    break;
  case 5:
    out->id[0] = '\0';
    break;
  case 6:
    out->peer_public_key = cleanup_save(storage, "wrong-peer");
    break;
  default:
    break;
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_rpc_friend_list(h2_gizclaw_service_t *service,
                                           h2_gizclaw_str_t cursor,
                                           size_t limit, uint32_t timeout,
                                           h2_gizclaw_resp_storage_t *storage,
                                           h2_gizclaw_friend_page_t *out) {
  assert(s_lookup_friend && service == s_business_fixture->actors[0].service);
  assert(limit == 64u && timeout == 15000u && storage != NULL &&
         storage->used == 0u);
  char input[256] = {0};
  assert(cursor.len < sizeof(input));
  if (cursor.len != 0u)
    memcpy(input, cursor.data, cursor.len);
  ++s_lookup_calls;
  if (s_lookup_rc != H2_PAL_OK)
    return s_lookup_rc;
  if (s_lookup_mode == LOOKUP_SECOND_ERROR && s_lookup_calls == 2u)
    return H2_PAL_ERR_IO;
  memset(storage->data, 0xA5, storage->capacity);
  *out = (h2_gizclaw_friend_page_t){0};
  const bool first = cursor.len == 0u;
  const bool page_only = s_lookup_mode == LOOKUP_REPEAT ||
                         s_lookup_mode == LOOKUP_CYCLE ||
                         (first && (s_lookup_mode == LOOKUP_SECOND_PAGE ||
                                    s_lookup_mode == LOOKUP_SECOND_ERROR ||
                                    s_lookup_mode == LOOKUP_LATE_TIMEOUT));
  if (page_only) {
    out->has_next = true;
    out->next_cursor = cleanup_save(storage, s_lookup_mode == LOOKUP_CYCLE &&
                                                     strcmp(input, "next") == 0
                                                 ? "other"
                                                 : "next");
    if (s_lookup_mode == LOOKUP_LATE_TIMEOUT)
      s_now += 45000u;
    return H2_PAL_OK;
  }
  if (s_lookup_mode == LOOKUP_ABSENT)
    return H2_PAL_OK;
  out->items = (h2_gizclaw_friend_t *)storage->data;
  storage->used = sizeof(*out->items);
  *out->items = (h2_gizclaw_friend_t){
      .id = cleanup_save(storage, s_business_fixture->actors[1].public_key),
      .peer_public_key =
          cleanup_save(storage, s_business_fixture->actors[1].public_key)};
  out->count = 1u;
  switch (s_lookup_mode) {
  case LOOKUP_DUPLICATE:
    if (first) {
      out->has_next = true;
      out->next_cursor = cleanup_save(storage, "next");
    }
    break;
  case LOOKUP_BAD_ID:
    out->items[0].id = cleanup_save(storage, "unrelated-peer");
    break;
  case LOOKUP_NULL_ITEMS:
    out->items = NULL;
    break;
  case LOOKUP_TOO_MANY:
    out->count = 65u;
    break;
  case LOOKUP_NULL_KEY:
    out->items[0].peer_public_key = NULL;
    break;
  case LOOKUP_UNTERMINATED:
    out->items[0].id = (char *)storage->data + storage->used;
    storage->data[storage->used++] = 'x';
    break;
  case LOOKUP_UNOWNED_ITEMS:
    out->items = (h2_gizclaw_friend_t *)&s_lookup_mode;
    break;
  case LOOKUP_UNALIGNED_ITEMS:
    out->items = (h2_gizclaw_friend_t *)(storage->data + 1u);
    break;
  case LOOKUP_BAD_CURSOR:
    out->has_next = true;
    out->next_cursor = "not-owned";
    break;
  case LOOKUP_LONG_CURSOR:
    out->has_next = true;
    out->next_cursor = (char *)storage->data + storage->used;
    memset(out->next_cursor, 'x', 256u);
    out->next_cursor[256] = '\0';
    storage->used += 257u;
    break;
  default:
    break;
  }
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_rpc_friend_invite_token_clear(h2_gizclaw_service_t *service,
                                         uint32_t timeout) {
  (void)service;
  (void)timeout;
  assert(false);
  return H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t h2_gizclaw_rpc_friend_group_invite_token_clear(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group, uint32_t timeout) {
  assert(s_owner_group && service == s_business_fixture->actors[0].service);
  assert(timeout == 15000u &&
         group.len == strlen(s_business_fixture->friend_group_name));
  assert(memcmp(group.data, s_business_fixture->friend_group_name, group.len) ==
         0);
  return s_group_invite_rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_group_member_delete(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group,
    h2_gizclaw_str_t member, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_friend_group_member_t *out) {
  assert(s_member_cleanup && service == s_business_fixture->actors[0].service);
  assert(group.len == strlen(s_business_fixture->friend_group_name));
  assert(!memcmp(group.data, s_business_fixture->friend_group_name, group.len));
  assert(member.len == strlen("membership-id") &&
         !memcmp(member.data, "membership-id", member.len));
  assert(timeout == 15000u && storage->used == 0u);
  ++s_member_deletes;
  if (s_member_mode == 27u)
    return H2_PAL_ERR_IO;
  if (s_member_mode == 28u)
    return H2_PAL_ERR_NOT_FOUND;
  *out = (h2_gizclaw_friend_group_member_t){
      .id = cleanup_save(storage, "membership-id"),
      .friend_group_name =
          cleanup_save(storage, s_business_fixture->friend_group_name),
      .peer_public_key =
          cleanup_save(storage, s_business_fixture->actors[2].public_key)};
  if (s_member_mode == 21u)
    out->id = cleanup_save(storage, "wrong-id");
  if (s_member_mode == 22u)
    out->friend_group_name = cleanup_save(storage, "wrong-group");
  if (s_member_mode == 23u)
    out->peer_public_key = cleanup_save(storage, "wrong-peer");
  if (s_member_mode == 24u)
    out->id = "unowned";
  if (s_member_mode == 25u)
    out->id = NULL;
  if (s_member_mode == 26u)
    storage->used = storage->capacity + 1u;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_rpc_friend_group_member_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_page_t *out) {
  assert(s_member_cleanup && service == s_business_fixture->actors[0].service);
  assert(group.len == strlen(s_business_fixture->friend_group_name));
  assert(!memcmp(group.data, s_business_fixture->friend_group_name, group.len));
  assert(limit == 64u && timeout == 15000u && !storage->used);
  ++s_member_lists;
  bool first = !cursor.len;
  if (s_member_mode == 16u || (s_member_mode == 17u && !first))
    return H2_PAL_ERR_IO;
  memset(out, 0, sizeof(*out));
  out->items = (void *)storage->data;
  storage->used = 2u * sizeof(*out->items);
  memset(out->items, 0, storage->used);
  bool next = ((s_member_mode == 1u || s_member_mode == 4u ||
                s_member_mode == 17u || s_member_mode == 18u) &&
               first) ||
              s_member_mode == 14u || s_member_mode == 15u;
  bool target = !(first && (s_member_mode == 1u || s_member_mode == 17u ||
                            s_member_mode == 18u)) &&
                s_member_mode != 14u && s_member_mode != 15u;
  out->count = s_member_mode == 2u ? 0u : 1u;
  *out->items = (h2_gizclaw_friend_group_member_t){
      .id = cleanup_save(storage, target ? "membership-id" : "another-id"),
      .friend_group_name =
          cleanup_save(storage, s_business_fixture->friend_group_name),
      .peer_public_key = cleanup_save(
          storage,
          target ? s_business_fixture->actors[2].public_key : "another-peer")};
  if (next) {
    out->has_next = true;
    out->next_cursor = cleanup_save(
        storage,
        s_member_mode == 15u && cursor.len == 1u && cursor.data[0] == 'a' ? "b"
        : s_member_mode == 15u                                            ? "a"
                               : "next");
  }
  if (s_member_mode == 3u) {
    out->count = 2u;
    out->items[1] = out->items[0];
  }
  if (s_member_mode == 5u)
    out->items = NULL;
  if (s_member_mode == 6u)
    out->count = 65u;
  if (s_member_mode == 7u)
    out->items = (void *)(storage->data + 1u);
  if (s_member_mode == 8u)
    out->items = (void *)&s_member_mode;
  if (s_member_mode == 9u)
    out->items[0].friend_group_name = cleanup_save(storage, "wrong");
  if (s_member_mode == 10u)
    out->items[0].peer_public_key = NULL;
  if (s_member_mode == 11u) {
    out->items[0].id = (char *)storage->data + storage->used;
    storage->data[storage->used++] = 'x';
  }
  if (s_member_mode == 12u) {
    out->items[0].id = (char *)storage->data + storage->used;
    memset(out->items[0].id, 'x',
           sizeof(s_business_fixture->friend_group_member_id));
    storage->used += sizeof(s_business_fixture->friend_group_member_id);
    storage->data[storage->used++] = '\0';
  }
  if (s_member_mode == 13u) {
    out->has_next = true;
    out->next_cursor = "unowned";
  }
  if (s_member_mode == 18u || s_member_mode == 29u)
    s_now += 45000u;
  return H2_PAL_OK;
}

static void *test_alloc(void *user, size_t len) {
  (void)user;
  return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
  (void)user;
  return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static h2_pal_result_t test_random(void *user, uint8_t *out, size_t len) {
  (void)user;
  memset(out, 0x6b, len);
  return H2_PAL_OK;
}

static h2_pal_result_t test_time(void *user, uint64_t *out_ms) {
  (void)user;
  *out_ms = s_now;
  return H2_PAL_OK;
}

static h2_pal_result_t test_sleep(void *user, uint32_t ms) {
  (void)user;
  s_now += ms;
  return H2_PAL_OK;
}

static h2_pal_result_t test_keypair(void *user, h2_pal_x25519_keypair_t *out) {
  (void)user;
  memset(out->private_key.bytes, 0x11, sizeof(out->private_key.bytes));
  memset(out->public_key.bytes, 0x22, sizeof(out->public_key.bytes));
  return H2_PAL_OK;
}

static h2_pal_result_t test_mutex_create(void *user,
                                         const h2_pal_mutex_config_t *config,
                                         h2_pal_mutex_t **out_mutex) {
  (void)user;
  (void)config;
  *out_mutex = (h2_pal_mutex_t *)&s_mutex_storage;
  return H2_PAL_OK;
}

static h2_pal_result_t test_mutex_operation(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  (void)mutex;
  return H2_PAL_OK;
}

static int test_log(void *user, h2_pal_log_level_t level, const char *scope,
                    const char *message) {
  (void)user;
  (void)level;
  (void)scope;
  (void)message;
  return H2_PAL_OK;
}

static int s_job_rc;
static int sync_job_body(void *ctx) {
  ++*(unsigned *)ctx;
  return s_job_rc;
}

static void test_call_sync(h2_runtime_t *runtime,
                           const h2_gizclaw_e2e_config_t *config) {
  static const h2_pal_task_vtable_t task_vtable = {
      .start = test_task_start,
      .join = test_task_join,
  };
  static const h2_pal_task_api_t task = {.vtable = &task_vtable};
  const h2_pal_task_api_t *saved_task = runtime->task;
  runtime->task = &task;
  h2_gizclaw_e2e_fixture_t fixture;
  assert(h2_gizclaw_e2e_fixture_init(&fixture, runtime, config, 600000u) ==
         H2_PAL_OK);
  struct h2_gizclaw_service service = {0};
  unsigned runs = 0u;
  assert(h2_gizclaw_e2e_fixture_call_sync(NULL, &service, sync_job_body,
                                          &runs) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_fixture_call_sync(&fixture, NULL, sync_job_body,
                                          &runs) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_fixture_call_sync(&fixture, &service, NULL, &runs) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(runs == 0u && s_task_starts == 0u);
  for (unsigned mode = 0u; mode < 7u; ++mode) {
    const unsigned polls_before = s_polls, stops_before = s_stops;
    s_task_starts = s_task_joins = s_join_failures = 0u;
    s_task_start_rc = s_job_rc = H2_PAL_OK;
    s_poll_fault = 0u;
    s_job_after_polls = 3u;
    runs = 0u;
    int expected = H2_PAL_OK;
    switch (mode) {
    case 0: /* task start failure: nothing runs, nothing polls */
      s_task_start_rc = H2_PAL_ERR_IO;
      expected = H2_PAL_ERR_IO;
      break;
    case 1: /* success: the job returns after the third App poll */
      break;
    case 2: /* job failure propagates */
      s_job_rc = H2_PAL_ERR_FORMAT;
      expected = H2_PAL_ERR_FORMAT;
      break;
    case 3: /* poll failure is reported once but dispatch continues */
      s_poll_fault = 3u;
      expected = H2_PAL_ERR_IO;
      break;
    case 4: /* deadline: the Service is stopped, cancelling the job */
      s_job_after_polls = 0u;
      assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 5u) == H2_PAL_OK);
      expected = H2_PAL_ERR_TIMEOUT;
      break;
    case 5: /* transient join failures are retried */
      s_join_failures = 2u;
      break;
    case 6: /* a join that never succeeds retains the task */
      s_join_failures = 1000u;
      expected = H2_PAL_ERR_BUSY;
      break;
    }
    const int rc =
        h2_gizclaw_e2e_fixture_call_sync(&fixture, &service, sync_job_body,
                                         &runs);
    assert(rc == expected);
    assert(s_task_starts == 1u);
    assert(s_job_entry == NULL);
    if (mode == 0u) {
      assert(runs == 0u && s_polls == polls_before && s_task_joins == 0u);
      continue;
    }
    assert(runs == 1u);
    if (mode == 4u) {
      assert(s_stops == stops_before + 1u && service.stopped);
      service.stopped = false;
      assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 600000u) ==
             H2_PAL_OK);
    } else {
      assert(s_polls == polls_before + 3u && s_stops == stops_before);
    }
    assert(s_task_joins == (mode == 5u ? 3u : mode == 6u ? 100u : 1u));
  }
  /* Mode 6 left a retained handle that the ledger reports and that blocks
   * release. A new call reclaims it first; while the join keeps failing the
   * call is refused without starting a second task. */
  assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 1u);
  runs = 0u;
  s_task_starts = 0u;
  const int busy =
      h2_gizclaw_e2e_fixture_call_sync(&fixture, &service, sync_job_body,
                                       &runs);
  assert(busy == H2_PAL_ERR_BUSY && runs == 0u && s_task_starts == 0u);
  assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 1u);
  /* Deinit must not free the fixture while the handle cannot be joined. */
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_ERR_BUSY);
  assert(fixture.retained_job_task != NULL);
  /* Once the join can complete, the retained task is reclaimed and release
   * succeeds. */
  s_join_failures = 0u;
  s_poll_fault = 0u;
  assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 1u);
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);
  assert(fixture.retained_job_task == NULL);
  runtime->task = saved_task;
}

int main(int argc, char **argv) {
  const h2_gizclaw_str_t empty = h2_gizclaw_e2e_str(NULL);
  assert(empty.data == NULL && empty.len == 0u);
  const h2_gizclaw_str_t value = h2_gizclaw_e2e_str("portable");
  assert(value.data != NULL && value.len == strlen("portable"));

  static const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .realloc = test_realloc,
      .free = test_free,
  };
  static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
  static const h2_pal_crypto_vtable_t crypto_vtable = {
      .random = test_random,
      .x25519_keypair_generate = test_keypair,
  };
  static const h2_pal_crypto_api_t crypto = {.vtable = &crypto_vtable};
  static const h2_pal_http_vtable_t http_vtable = {0};
  static const h2_pal_http_api_t http = {.vtable = &http_vtable};
  static const h2_pal_log_vtable_t log_vtable = {.write = test_log};
  static const h2_pal_log_api_t log = {.vtable = &log_vtable};
  static const h2_pal_time_vtable_t time_vtable = {
      .get_monotonic_ms = test_time,
      .sleep_ms = test_sleep,
  };
  static const h2_pal_time_api_t time = {.vtable = &time_vtable};
  static const h2_pal_sync_vtable_t sync_vtable = {
      .create_mutex = test_mutex_create,
      .destroy_mutex = test_mutex_operation,
      .lock_mutex = test_mutex_operation,
      .try_lock_mutex = test_mutex_operation,
      .unlock_mutex = test_mutex_operation,
  };
  static const h2_pal_sync_api_t sync = {.vtable = &sync_vtable};
  static const h2_pal_webrtc_vtable_t webrtc_vtable = {0};
  static const h2_pal_webrtc_api_t webrtc = {.vtable = &webrtc_vtable};
  static const h2_pal_task_api_t task = {0};
  static const h2_pal_queue_api_t queue = {0};
  h2_runtime_t runtime = {
      .mem = &mem,
      .log = &log,
      .time = &time,
      .sync = &sync,
      .crypto = &crypto,
      .http = &http,
      .webrtc = &webrtc,
      .task = &task,
      .queue = &queue,
  };

  static const char endpoint[] = "e2e.gizclaw.com:9821";
  char token[] = "borrowed-token";
  const h2_gizclaw_e2e_config_t config = {
      .server_endpoint = {endpoint, sizeof(endpoint) - 1u},
      .registration_token = {token, sizeof(token) - 1u},
      .should_stop = user_stop,
  };
  h2_gizclaw_e2e_fixture_t fixture;
  if (argc == 3 && strcmp(argv[1], "--emit-lifecycle-evidence") == 0) {
    const int mode = atoi(argv[2]);
    assert(mode >= 0 && mode <= 10);
    printf("H2_GIZCLAW_E2E stage=coverage-begin case=service\n");
    assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 600000u) ==
           H2_PAL_OK);
    s_unterminated_profile = mode == 1;
    s_null_service = mode == 9;
    s_start_rc = mode == 10 ? H2_PAL_ERR_IO : H2_PAL_OK;
    if (mode == 2)
      s_profile = "";
    if (mode == 3)
      s_register_rc = H2_PAL_ERR_TIMEOUT;
    int rc = h2_gizclaw_e2e_fixture_connect_actors(&fixture, 1u);
    const int expected_connect = mode == 1 || mode == 2 ? H2_PAL_ERR_FORMAT
                                 : mode == 3            ? H2_PAL_ERR_TIMEOUT
                                 : mode == 9  ? H2_PAL_ERR_INVALID_STATE
                                 : mode == 10 ? H2_PAL_ERR_IO
                                              : H2_PAL_OK;
    assert(rc == expected_connect);
    assert(fixture.actors[0].registered == (rc == H2_PAL_OK));
    if (rc != H2_PAL_OK)
      assert(fixture.runtime_profile_name[0] == '\0');
    assert(fixture.actors[0].peer_delete_required == (mode != 9));
    /* A valid retry is required for cleanup after a rejected registration. */
    s_unterminated_profile = false;
    s_null_service = false;
    s_start_rc = H2_PAL_OK;
    s_profile = "runtime-profile-from-server";
    s_register_rc = H2_PAL_OK;
    assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
    s_stop_rc = mode == 4 ? H2_PAL_ERR_TIMEOUT : H2_PAL_OK;
    s_deinit_rc = mode == 5 ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
    s_poll_fault = mode >= 6 && mode <= 8 ? (unsigned)(mode - 5) : 0u;
    int cleanup_rc = h2_gizclaw_e2e_fixture_deinit(&fixture);
    const int expected_cleanup = mode == 4 ? H2_PAL_ERR_TIMEOUT
                                 : mode >= 5 && mode <= 7
                                     ? H2_PAL_ERR_INVALID_STATE
                                 : mode == 8 ? H2_PAL_ERR_IO
                                             : H2_PAL_OK;
    assert(cleanup_rc == expected_cleanup);
    if (cleanup_rc != H2_PAL_OK) {
      assert(s_live_services == 1u && fixture.actors[0].service != NULL);
      assert(fixture.registration_token != NULL &&
             fixture.actors[0].private_key[0] != '\0');
    } else {
      assert(s_live_services == 0u && fixture.actors[0].service == NULL);
      assert(fixture.registration_token == NULL);
    }
    printf("H2_GIZCLAW_E2E stage=coverage-end case=service status=%s rc=%d "
           "cleanup_rc=%d\n",
           rc == H2_PAL_OK && cleanup_rc == H2_PAL_OK ? "PASS" : "FAIL", rc,
           cleanup_rc);
    s_stop_rc = s_deinit_rc = H2_PAL_OK;
    s_poll_fault = 0u;
    /* Retry only after the failed evidence span has closed; recovery must not
     * retroactively certify the failed lifecycle attempt. */
    assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);
    assert(s_live_services == 0u);
    return 0;
  }
  assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 1000u) ==
         H2_PAL_OK);
  assert(fixture.registration_token != token);
  assert(strcmp(fixture.registration_token, "borrowed-token") == 0);
  token[0] = 'X';
  assert(strcmp(fixture.registration_token, "borrowed-token") == 0);
  assert(strcmp(fixture.endpoint, endpoint) == 0);
  assert(strcmp(fixture.workspace_name, "h2e2e-6b6b6b6b6b6b6b6b-workspace") ==
         0);
  assert(strcmp(fixture.pet_name, "h2e2e-6b6b6b6b6b6b6b6b-pet") == 0);
  assert(fixture.runtime_profile_name[0] == '\0');
  assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 0u);
  assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 1u) == H2_PAL_OK);
  assert(s_starts == 1u && s_registers == 1u);
  h2_gizclaw_service_t *original_service = fixture.actors[0].service;
  assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 1000u) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(fixture.actors[0].service == original_service);
  assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 1u) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(s_starts == 1u && s_registers == 1u);
  assert(strcmp(fixture.runtime_profile_name, s_profile) == 0);
  assert(fixture.actors[0].service->config.client_config ==
         &fixture.actors[0].config);
  assert(fixture.actors[0].config.private_key.data ==
         fixture.actors[0].private_key);
  assert(h2_gizclaw_e2e_fixture_poll(&fixture, H2_GIZCLAW_E2E_OWNER, 3u) ==
         H2_PAL_OK);
  assert(s_polls == 4u);
  assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 1000u) == H2_PAL_OK);
  s_delete_rc = H2_PAL_ERR_IO;
  s_user_stop = true;
  assert(fixture.actors[0].config.cancel_requested(&fixture));
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_ERR_IO);
  assert(!fixture.actors[0].config.cancel_requested(&fixture));
  s_user_stop = false;
  assert(fixture.actors[0].peer_delete_required);
  assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 1u);
  s_delete_rc = H2_PAL_OK;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
  assert(!fixture.actors[0].peer_delete_required && s_deletes == 2u);
  assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 0u);
  s_stop_rc = H2_PAL_ERR_TIMEOUT;
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_ERR_TIMEOUT);
  assert(fixture.registration_token != NULL &&
         fixture.actors[0].service != NULL);
  assert(fixture.actors[0].private_key[0] != '\0');
  s_stop_rc = H2_PAL_OK;
  s_deinit_rc = H2_PAL_ERR_INVALID_STATE;
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_ERR_INVALID_STATE);
  assert(fixture.registration_token != NULL);
  s_deinit_rc = H2_PAL_OK;
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);

  token[0] = 'b';
  assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 1000u) ==
         H2_PAL_OK);
  s_register_rc = H2_PAL_ERR_TIMEOUT;
  assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 1u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(fixture.actors[0].peer_delete_required &&
         !fixture.actors[0].registered);
  char identity[sizeof(fixture.actors[0].private_key)];
  memcpy(identity, fixture.actors[0].private_key, sizeof(identity));
  s_register_rc = H2_PAL_OK;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
  assert(strcmp(identity, fixture.actors[0].private_key) == 0);
  assert(!fixture.actors[0].peer_delete_required);
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);

  assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 1000u) ==
         H2_PAL_OK);
  s_start_rc = H2_PAL_ERR_IO;
  assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 1u) == H2_PAL_ERR_IO);
  assert(fixture.actors[0].peer_delete_required);
  s_start_rc = H2_PAL_OK;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
  assert(!fixture.actors[0].peer_delete_required);
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);
  /* A failed second-peer deletion must survive the domain's stack and keep
   * that peer registered until the retry succeeds. */
  assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 1000u) ==
         H2_PAL_OK);
  assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 2u) == H2_PAL_OK);
  strcpy(fixture.contact_name, "isolated-contact");
  strcpy(fixture.friend_group_name, "isolated-group");
  fixture.isolation_contact_pending = fixture.isolation_group_pending = true;
  fixture.isolation_pet_pending = fixture.isolation_workspace_pending = true;
  assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 45000u) == H2_PAL_OK);
  s_business_fixture = &fixture;
  s_business_allowed = CONTACT | GROUP | PET | WORKSPACE;
  s_business_failed = GROUP;
  const unsigned before_deletes = s_deletes;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_ERR_IO);
  assert(s_business_seen == s_business_allowed);
  assert(fixture.isolation_group_pending && fixture.actors[1].registered);
  assert(s_deletes == before_deletes + 1u);
  assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 2u);
  s_business_failed = s_business_seen = 0u;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
  assert(s_business_seen == GROUP && s_deletes == before_deletes + 2u);
  assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 0u);
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);

  /* An add can take effect remotely even when its response/ID is lost. */
  assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 1000u) ==
         H2_PAL_OK);
  assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 2u) == H2_PAL_OK);
  assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 45000u) == H2_PAL_OK);
  fixture.friendship_created = true;
  s_lookup_friend = true;
  s_lookup_rc = H2_PAL_ERR_IO;
  s_business_allowed = FRIEND;
  s_business_seen = 0u;
  const unsigned before_lookup = s_deletes;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_ERR_IO);
  assert(fixture.friendship_created && fixture.friend_id[0] == '\0');
  assert(s_business_seen == 0u && s_deletes == before_lookup);
  s_lookup_rc = H2_PAL_OK;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
  assert(strcmp(fixture.friend_id, fixture.actors[1].public_key) == 0);
  assert(s_business_seen == FRIEND && s_deletes == before_lookup + 2u);
  assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 0u);
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);

  for (unsigned mode = LOOKUP_SECOND_PAGE; mode <= LOOKUP_LATE_TIMEOUT;
       ++mode) {
    assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 1000u) ==
           H2_PAL_OK);
    assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 2u) == H2_PAL_OK);
    assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 45000u) == H2_PAL_OK);
    fixture.friendship_created = true;
    s_lookup_mode = mode;
    s_lookup_calls = 0u;
    s_business_seen = 0u;
    const unsigned before = s_deletes;
    int rc = h2_gizclaw_e2e_fixture_cleanup(&fixture);
    if (mode == LOOKUP_SECOND_PAGE) {
      assert(rc == H2_PAL_OK && s_lookup_calls == 2u &&
             !fixture.friendship_created);
      assert(s_business_seen == FRIEND && s_deletes == before + 2u);
    } else {
      assert(rc != H2_PAL_OK && fixture.friendship_created &&
             fixture.friend_id[0] == '\0');
      assert(s_business_seen == 0u && s_deletes == before);
      if (mode == LOOKUP_ABSENT)
        assert(rc == H2_PAL_ERR_NOT_FOUND);
      if (mode == LOOKUP_CYCLE)
        assert(rc == H2_PAL_ERR_NO_SPACE && s_lookup_calls == 32u);
      if (mode == LOOKUP_SECOND_ERROR)
        assert(rc == H2_PAL_ERR_IO && s_lookup_calls == 2u);
      if (mode == LOOKUP_LATE_TIMEOUT)
        assert(rc == H2_PAL_ERR_TIMEOUT && s_lookup_calls == 1u);
      s_lookup_mode = LOOKUP_NORMAL;
      s_lookup_calls = 0u;
      assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 45000u) ==
             H2_PAL_OK);
      assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
      assert(s_deletes == before + 2u);
    }
    assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);
  }
  s_lookup_mode = LOOKUP_NORMAL;
  for (unsigned mode = 1u; mode <= 7u; ++mode) {
    assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 1000u) ==
           H2_PAL_OK);
    assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 2u) == H2_PAL_OK);
    assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 45000u) == H2_PAL_OK);
    fixture.friendship_created = true;
    strcpy(fixture.friend_id, fixture.actors[1].public_key);
    s_friend_delete_mode = mode;
    s_business_seen = 0u;
    const unsigned before = s_deletes;
    if (mode == 7u)
      assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 1000u) == H2_PAL_OK);
    assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) != H2_PAL_OK);
    assert(fixture.friendship_created && s_deletes == before);
    assert(s_business_seen == (mode == 7u ? 0u : FRIEND));
    s_friend_delete_mode = 0u;
    assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 45000u) == H2_PAL_OK);
    assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
    assert(!fixture.friendship_created && s_deletes == before + 2u);
    assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);
  }

  /* Retain a parent group while invitation cleanup is still pending. */
  assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 1000u) ==
         H2_PAL_OK);
  assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 1u) == H2_PAL_OK);
  strcpy(fixture.friend_group_name, "isolated-group");
  fixture.friend_group_created = fixture.friend_group_invite_created = true;
  s_owner_group = true;
  s_group_invite_rc = H2_PAL_ERR_IO;
  s_business_allowed = GROUP;
  s_business_seen = 0u;
  const unsigned before_group = s_deletes;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_ERR_IO);
  assert(s_business_seen == 0u && s_deletes == before_group);
  assert(fixture.friend_group_created && fixture.actors[0].registered);
  s_group_invite_rc = H2_PAL_OK;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
  assert(s_business_seen == GROUP && s_deletes == before_group + 1u);
  assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 0u);
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);
  /* Retained Track/hooks must be drained before business resources are deleted.
   * Failed draining keeps their service, identity and borrowed storage alive.
   */
  assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 1000u) ==
         H2_PAL_OK);
  assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 1u) == H2_PAL_OK);
  fixture.case_state = &s_case_cleanups;
  fixture.case_cleanup = cleanup_case;
  s_case_cleanup_rc = H2_PAL_ERR_TIMEOUT;
  const unsigned before_case_delete = s_deletes;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_ERR_TIMEOUT);
  assert(s_case_cleanups == 1u && s_deletes == before_case_delete);
  assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 2u);
  s_case_cleanup_rc = H2_PAL_OK;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
  assert(s_case_cleanups == 2u && s_deletes == before_case_delete + 1u);
  /* Deinit stops publishers first, but must not destroy the service until the
   * retained case successfully releases its references. */
  fixture.case_state = &s_case_cleanups;
  fixture.case_cleanup = cleanup_case;
  s_stop_rc = H2_PAL_ERR_TIMEOUT;
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_ERR_TIMEOUT);
  assert(s_case_cleanups == 2u && fixture.case_state != NULL);
  s_stop_rc = H2_PAL_OK;
  s_case_expect_stopped = true;
  s_case_cleanup_rc = H2_PAL_ERR_IO;
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_ERR_IO);
  assert(s_case_cleanups == 3u && fixture.case_state != NULL);
  assert(fixture.actors[0].service != NULL &&
         fixture.registration_token != NULL);
  s_case_cleanup_rc = H2_PAL_OK;
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);
  assert(s_case_cleanups == 4u && fixture.case_state == NULL);
  s_case_expect_stopped = false;
  /* A successful transport result with the wrong deletion identity must not
   * clear the obligation or delete the owning Peer. */
  assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 45000u) ==
         H2_PAL_OK);
  assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 1u) == H2_PAL_OK);
  fixture.workspace_created = true;
  s_business_allowed = WORKSPACE;
  s_business_failed = 0u;
  s_workspace_role = H2_GIZCLAW_E2E_OWNER;
  s_workspace_mode = 1u;
  const unsigned before_workspace_peer = s_deletes;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_ERR_FORMAT);
  assert(fixture.workspace_created && s_deletes == before_workspace_peer);
  s_workspace_mode = 0u;
  assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
  assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);
  /* Both actors use the same name but own separate cleanup obligations. */
  static const unsigned workspace_cleanup_modes[] = {
      0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 16u, 17u};
  for (unsigned role = 0u; role < 2u; ++role) {
    for (size_t mode_index = 0u;
         mode_index <
         sizeof(workspace_cleanup_modes) / sizeof(workspace_cleanup_modes[0]);
         ++mode_index) {
      const unsigned mode = workspace_cleanup_modes[mode_index];
      assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 45000u) ==
             H2_PAL_OK);
      assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 2u) == H2_PAL_OK);
      bool *pending = role ? &fixture.isolation_workspace_pending
                           : &fixture.workspace_created;
      bool *ack = role ? &fixture.isolation_workspace_delete_acknowledged
                       : &fixture.workspace_delete_acknowledged;
      *pending = true;
      *ack = mode == 17u;
      s_business_allowed = WORKSPACE;
      s_business_failed = 0u;
      s_workspace_role = role;
      s_workspace_mode = mode;
      s_workspace_deletes = s_workspace_gets = 0u;
      if (mode == 16u)
        assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 1u) == H2_PAL_OK);
      const unsigned before = s_deletes;
      int rc = h2_gizclaw_e2e_fixture_cleanup(&fixture);
      const int expected = mode == 0u || mode == 17u    ? H2_PAL_OK
                           : mode == 8u                 ? H2_PAL_ERR_NOT_FOUND
                           : mode == 9u || mode == 11u  ? H2_PAL_ERR_IO
                           : mode == 10u || mode == 16u ? H2_PAL_ERR_TIMEOUT
                           : mode == 12u ? H2_PAL_ERR_INVALID_STATE
                                         : H2_PAL_ERR_FORMAT;
      assert(rc == expected);
      assert(s_workspace_deletes == ((mode == 16u || mode == 17u) ? 0u : 1u));
      assert(s_workspace_gets == 0u);
      const bool expected_pending = (mode >= 1u && mode <= 9u) || mode == 16u;
      assert(*pending == expected_pending);
      assert(!*ack);
      assert(s_deletes == before + (rc == H2_PAL_OK ? 2u
                                    : mode == 10u   ? 0u
                                                    : 1u));
      if (rc != H2_PAL_OK) {
        assert(fixture.actors[role].registered &&
               fixture.actors[role].peer_delete_required);
        assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 2u);
        const unsigned expected_deletes =
            s_workspace_deletes + (*pending ? 1u : 0u);
        s_workspace_mode = 0u;
        assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 45000u) ==
               H2_PAL_OK);
        assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
        assert(s_workspace_deletes == expected_deletes);
        assert(!*pending && !*ack && s_deletes == before + 2u);
      }
      assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 0u);
      assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);
    }
  }
  for (unsigned role = 0u; role < 2u; ++role) {
    for (unsigned mode = 0u; mode <= 18u; ++mode) {
      assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 45000u) ==
             H2_PAL_OK);
      assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 2u) == H2_PAL_OK);
      bool *pending =
          role ? &fixture.isolation_pet_pending : &fixture.pet_created;
      bool *ack = role ? &fixture.isolation_pet_delete_acknowledged
                       : &fixture.pet_delete_acknowledged;
      *pending = true;
      *ack = mode == 17u;
      s_business_allowed = PET;
      s_business_failed = 0u;
      s_pet_role = role;
      s_pet_mode = mode;
      s_pet_deletes = s_pet_gets = 0u;
      if (mode == 16u)
        assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 1u) == H2_PAL_OK);
      const unsigned before = s_deletes;
      int rc = h2_gizclaw_e2e_fixture_cleanup(&fixture);
      int expected = mode == 0u || mode == 17u || mode == 18u ? H2_PAL_OK
                     : mode == 6u               ? H2_PAL_ERR_NOT_FOUND
                     : mode == 7u || mode == 9u ? H2_PAL_ERR_IO
                     : mode == 10u || mode == 12u || mode == 16u
                         ? H2_PAL_ERR_TIMEOUT
                         : H2_PAL_ERR_FORMAT;
      assert(rc == expected);
      assert(s_pet_deletes == ((mode == 16u || mode == 17u) ? 0u : 1u));
      unsigned gets = mode == 12u   ? 32u
                      : mode == 18u ? 3u
                      : (mode == 0u || mode == 9u ||
                         (mode >= 11u && mode <= 15u) || mode == 17u)
                          ? 1u
                          : 0u;
      assert(s_pet_gets == gets);
      assert(*pending == (rc != H2_PAL_OK));
      assert(*ack == (mode >= 9u && mode <= 15u));
      assert(s_deletes == before + (rc == H2_PAL_OK ? 2u
                                    : mode == 10u   ? 0u
                                                    : 1u));
      if (rc != H2_PAL_OK) {
        assert(fixture.actors[role].registered &&
               fixture.actors[role].peer_delete_required);
        assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) ==
               (mode == 10u ? 3u : 2u));
        unsigned expected_deletes = s_pet_deletes + (*ack ? 0u : 1u);
        s_pet_mode = 0u;
        assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 45000u) ==
               H2_PAL_OK);
        assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
        assert(s_pet_deletes == expected_deletes && !*pending && !*ack);
        assert(s_deletes == before + 2u);
      }
      assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 0u);
      assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);
    }
  }
  s_member_cleanup = true;
  for (unsigned mode = 0u; mode <= 29u; ++mode) {
    assert(h2_gizclaw_e2e_fixture_init(&fixture, &runtime, &config, 45000u) ==
           H2_PAL_OK);
    assert(h2_gizclaw_e2e_fixture_connect_actors(&fixture, 3u) == H2_PAL_OK);
    fixture.friend_group_created = fixture.friend_group_member_joined = true;
    strcpy(fixture.friend_group_name, "test-group");
    fixture.friend_group_member_id[0] = '\0';
    if (mode == 20u)
      strcpy(fixture.friend_group_member_id, "membership-id");
    s_business_fixture = &fixture;
    s_business_allowed = GROUP;
    s_business_failed = s_business_seen = 0u;
    s_owner_group = true;
    s_member_mode = mode;
    s_member_lists = s_member_deletes = 0u;
    if (mode == 19u)
      assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 1u) == H2_PAL_OK);
    unsigned before = s_deletes;
    int rc = h2_gizclaw_e2e_fixture_cleanup(&fixture);
    bool success = mode == 0u || mode == 1u || mode == 20u;
    assert((rc == H2_PAL_OK) == success);
    if (success) {
      assert(!fixture.friend_group_created &&
             !fixture.friend_group_member_joined);
      assert(s_member_deletes == 1u && s_deletes == before + 3u);
      assert(s_member_lists == (mode == 20u ? 0u : mode == 1u ? 2u : 1u));
    } else {
      assert(fixture.friend_group_created &&
             fixture.friend_group_member_joined);
      assert(s_business_seen == 0u);
      assert(s_deletes == before + (mode == 18u || mode == 29u ? 0u : 1u));
      assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) ==
             (mode == 18u || mode == 29u ? 5u : 4u));
      assert((fixture.friend_group_member_id[0] != '\0') == (mode >= 21u));
      assert(s_member_lists <= 32u);
      if (mode == 15u)
        assert(s_member_lists == 32u);
      if (mode == 29u)
        assert(s_member_deletes == 0u);
      s_member_mode = 0u;
      assert(h2_gizclaw_e2e_fixture_set_deadline(&fixture, 45000u) ==
             H2_PAL_OK);
      assert(h2_gizclaw_e2e_fixture_cleanup(&fixture) == H2_PAL_OK);
      assert(!fixture.friend_group_created &&
             !fixture.friend_group_member_joined);
      assert(s_deletes == before + 3u);
    }
    assert(h2_gizclaw_e2e_fixture_emit_recovery_ledger(&fixture) == 0u);
    assert(h2_gizclaw_e2e_fixture_deinit(&fixture) == H2_PAL_OK);
  }
  test_call_sync(&runtime, &config);
  return 0;
}
