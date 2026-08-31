#define H2_GIZCLAW_INTERNAL_SYNC_API
#include "h2_gizclaw_social.h"
#undef H2_GIZCLAW_INTERNAL_SYNC_API
#include "h2_gizclaw_service_internal.h"

#include <stdatomic.h>
#include <string.h>

struct h2_gizclaw_social_request {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_operation_t *operation;
  const h2_gizclaw_cancel_token_t *cancel_token;
  h2_gizclaw_social_request_kind_t kind;
  h2_gizclaw_social_completion_fn completion;
  void *completion_user;
  h2_gizclaw_social_result_t result;
  char *text[3];
  size_t limit;
  h2_gizclaw_friend_group_role_t role;
  h2_gizclaw_friend_group_message_audio_write_fn write;
  void *write_user;
  atomic_bool terminal;
};

static h2_gizclaw_str_t social_span(const char *text) {
  return (h2_gizclaw_str_t){
      .data = text, .len = text == NULL ? 0u : strlen(text)};
}

static bool social_text_valid(h2_gizclaw_str_t text, bool required) {
  return (!required || text.len > 0u) &&
         (text.len == 0u || text.data != NULL);
}

static char *social_copy(const h2_pal_mem_api_t *allocator,
                         h2_gizclaw_str_t text) {
  if (!social_text_valid(text, false))
    return NULL;
  char *copy = h2_pal_mem_alloc(allocator, text.len + 1u);
  if (copy == NULL)
    return NULL;
  if (text.len > 0u)
    memcpy(copy, text.data, text.len);
  copy[text.len] = '\0';
  return copy;
}

static void social_contact_clear(const h2_pal_mem_api_t *allocator,
                                 h2_gizclaw_contact_t *value) {
  h2_pal_mem_free(allocator, value->name);
  h2_pal_mem_free(allocator, value->display_name);
  h2_pal_mem_free(allocator, value->phone_number);
  h2_pal_mem_free(allocator, value->created_at);
  h2_pal_mem_free(allocator, value->updated_at);
  memset(value, 0, sizeof(*value));
}

static void social_group_clear(const h2_pal_mem_api_t *allocator,
                               h2_gizclaw_friend_group_t *value) {
  h2_pal_mem_free(allocator, value->name);
  h2_pal_mem_free(allocator, value->display_name);
  h2_pal_mem_free(allocator, value->description);
  h2_pal_mem_free(allocator, value->workspace_name);
  memset(value, 0, sizeof(*value));
}

static void social_friend_clear(const h2_pal_mem_api_t *allocator,
                                h2_gizclaw_friend_t *value) {
  h2_pal_mem_free(allocator, value->id);
  h2_pal_mem_free(allocator, value->peer_public_key);
  h2_pal_mem_free(allocator, value->workspace_name);
  h2_pal_mem_free(allocator, value->created_at);
  h2_pal_mem_free(allocator, value->updated_at);
  h2_pal_mem_free(allocator, value->name);
  h2_pal_mem_free(allocator, value->emoji);
  memset(value, 0, sizeof(*value));
}

static void social_member_clear(const h2_pal_mem_api_t *allocator,
                                h2_gizclaw_friend_group_member_t *value) {
  h2_pal_mem_free(allocator, value->id);
  h2_pal_mem_free(allocator, value->friend_group_name);
  h2_pal_mem_free(allocator, value->peer_public_key);
  h2_pal_mem_free(allocator, value->created_at);
  h2_pal_mem_free(allocator, value->updated_at);
  memset(value, 0, sizeof(*value));
}

static void social_message_clear(const h2_pal_mem_api_t *allocator,
                                 h2_gizclaw_friend_group_message_t *value) {
  h2_pal_mem_free(allocator, value->history_id);
  h2_pal_mem_free(allocator, value->friend_group_name);
  h2_pal_mem_free(allocator, value->sender_peer_public_key);
  h2_pal_mem_free(allocator, value->created_at);
  h2_pal_mem_free(allocator, value->expires_at);
  h2_pal_mem_free(allocator, value->name);
  h2_pal_mem_free(allocator, value->text);
  memset(value, 0, sizeof(*value));
}

static void social_result_clear(h2_gizclaw_social_request_t *request) {
  switch (request->kind) {
  case H2_GIZCLAW_SOCIAL_CONTACTS_LIST:
    for (size_t index = 0u; index < request->result.value.contact_page.count;
         ++index)
      social_contact_clear(request->allocator,
                           &request->result.value.contact_page.items[index]);
    h2_pal_mem_free(request->allocator,
                    request->result.value.contact_page.items);
    h2_pal_mem_free(request->allocator,
                    request->result.value.contact_page.next_cursor);
    memset(&request->result.value.contact_page, 0,
           sizeof(request->result.value.contact_page));
    break;
  case H2_GIZCLAW_SOCIAL_CONTACT_GET:
  case H2_GIZCLAW_SOCIAL_CONTACT_CREATE:
  case H2_GIZCLAW_SOCIAL_CONTACT_PUT:
  case H2_GIZCLAW_SOCIAL_CONTACT_DELETE:
    social_contact_clear(request->allocator, &request->result.value.contact);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUPS_LIST:
    for (size_t index = 0u;
         index < request->result.value.friend_group_page.count; ++index)
      social_group_clear(
          request->allocator,
          &request->result.value.friend_group_page.items[index]);
    h2_pal_mem_free(request->allocator,
                    request->result.value.friend_group_page.items);
    h2_pal_mem_free(request->allocator,
                    request->result.value.friend_group_page.next_cursor);
    memset(&request->result.value.friend_group_page, 0,
           sizeof(request->result.value.friend_group_page));
    break;
  case H2_GIZCLAW_SOCIAL_FRIENDS_LIST:
    for (size_t index = 0u; index < request->result.value.friend_page.count;
         ++index)
      social_friend_clear(request->allocator,
                          &request->result.value.friend_page.items[index]);
    h2_pal_mem_free(request->allocator, request->result.value.friend_page.items);
    h2_pal_mem_free(request->allocator,
                    request->result.value.friend_page.next_cursor);
    memset(&request->result.value.friend_page, 0,
           sizeof(request->result.value.friend_page));
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INFO_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_ADD:
  case H2_GIZCLAW_SOCIAL_FRIEND_DELETE:
    social_friend_clear(request->allocator,
                        &request->result.value.friend_value);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CREATE:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CREATE:
    h2_pal_mem_free(request->allocator,
                    request->result.value.invite_token.value);
    h2_pal_mem_free(request->allocator,
                    request->result.value.invite_token.expires_at);
    memset(&request->result.value.invite_token, 0,
           sizeof(request->result.value.invite_token));
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_CREATE:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_PUT:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_DELETE:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_JOIN:
    social_group_clear(request->allocator,
                       &request->result.value.friend_group);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBERS_LIST:
    for (size_t index = 0u; index < request->result.value.member_page.count;
         ++index)
      social_member_clear(request->allocator,
                          &request->result.value.member_page.items[index]);
    h2_pal_mem_free(request->allocator, request->result.value.member_page.items);
    h2_pal_mem_free(request->allocator,
                    request->result.value.member_page.next_cursor);
    memset(&request->result.value.member_page, 0,
           sizeof(request->result.value.member_page));
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_PUT:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_DELETE:
    social_member_clear(request->allocator, &request->result.value.member);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGES_LIST:
    for (size_t index = 0u; index < request->result.value.message_page.count;
         ++index)
      social_message_clear(request->allocator,
                           &request->result.value.message_page.items[index]);
    h2_pal_mem_free(request->allocator,
                    request->result.value.message_page.items);
    h2_pal_mem_free(request->allocator,
                    request->result.value.message_page.next_cursor);
    memset(&request->result.value.message_page, 0,
           sizeof(request->result.value.message_page));
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_GET:
    social_message_clear(request->allocator, &request->result.value.message);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_AUDIO_DOWNLOAD:
    h2_pal_mem_free(request->allocator,
                    request->result.value.message_audio.friend_group_name);
    h2_pal_mem_free(request->allocator,
                    request->result.value.message_audio.history_id);
    h2_pal_mem_free(request->allocator,
                    request->result.value.message_audio.mime_type);
    memset(&request->result.value.message_audio, 0,
           sizeof(request->result.value.message_audio));
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CLEAR:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CLEAR:
    break;
  }
}

typedef struct social_write_dispatch {
  h2_gizclaw_social_request_t *request;
  const uint8_t *data;
  size_t len;
} social_write_dispatch_t;

static h2_pal_result_t social_dispatch_write(void *user) {
  social_write_dispatch_t *dispatch = user;
  return (h2_pal_result_t)dispatch->request->write(
      dispatch->request->write_user, dispatch->data, dispatch->len);
}

static int social_write(void *user, const uint8_t *data, size_t len) {
  h2_gizclaw_social_request_t *request = user;
  social_write_dispatch_t dispatch = {
      .request = request, .data = data, .len = len};
  return (int)h2_gizclaw_operation_dispatch_call(
      request->cancel_token, social_dispatch_write, &dispatch);
}

static h2_pal_result_t social_run(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_social_request_t *request = user;
  const h2_gizclaw_str_t a = social_span(request->text[0]);
  const h2_gizclaw_str_t b = social_span(request->text[1]);
  const h2_gizclaw_str_t c = social_span(request->text[2]);
  request->cancel_token = cancel_token;
  h2_pal_result_t rc = H2_PAL_ERR_INVALID_STATE;
  switch (request->kind) {
  case H2_GIZCLAW_SOCIAL_CONTACTS_LIST:
    rc = (h2_pal_result_t)h2_gizclaw_client_contacts_list(
        client, a, request->limit, &request->result.value.contact_page);
    break;
  case H2_GIZCLAW_SOCIAL_CONTACT_GET:
    rc = (h2_pal_result_t)h2_gizclaw_client_contact_get(
        client, a, &request->result.value.contact);
    break;
  case H2_GIZCLAW_SOCIAL_CONTACT_CREATE:
    rc = (h2_pal_result_t)h2_gizclaw_client_contact_create(
        client, a, b, c, &request->result.value.contact);
    break;
  case H2_GIZCLAW_SOCIAL_CONTACT_PUT:
    rc = (h2_pal_result_t)h2_gizclaw_client_contact_put(
        client, a, b, c, &request->result.value.contact);
    break;
  case H2_GIZCLAW_SOCIAL_CONTACT_DELETE:
    rc = (h2_pal_result_t)h2_gizclaw_client_contact_delete(
        client, a, &request->result.value.contact);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUPS_LIST:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_groups_list(
        client, a, request->limit, &request->result.value.friend_group_page);
    break;
  case H2_GIZCLAW_SOCIAL_FRIENDS_LIST:
    rc = (h2_pal_result_t)h2_gizclaw_client_friends_list(
        client, a, request->limit, &request->result.value.friend_page);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INFO_GET:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_info_get(
        client, a, &request->result.value.friend_value);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_ADD:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_add(
        client, a, &request->result.value.friend_value);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_DELETE:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_delete(
        client, a, &request->result.value.friend_value);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_GET:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_invite_token_get(
        client, &request->result.value.invite_token);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CREATE:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_invite_token_create(
        client, &request->result.value.invite_token);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CLEAR:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_invite_token_clear(client);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_GET:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_get(
        client, a, &request->result.value.friend_group);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_CREATE:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_create(
        client, a, b, c, &request->result.value.friend_group);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_PUT:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_put(
        client, a, b, c, &request->result.value.friend_group);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_DELETE:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_delete(
        client, a, &request->result.value.friend_group);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_JOIN:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_join(
        client, a, b, &request->result.value.friend_group);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_GET:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_invite_token_get(
        client, a, &request->result.value.invite_token);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CREATE:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_invite_token_create(
        client, a, &request->result.value.invite_token);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CLEAR:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_invite_token_clear(
        client, a);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBERS_LIST:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_members_list(
        client, a, b, request->limit, &request->result.value.member_page);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_PUT:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_member_put(
        client, a, b, request->role, &request->result.value.member);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_DELETE:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_member_delete(
        client, a, b, &request->result.value.member);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGES_LIST:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_messages_list(
        client, a, b, request->limit, &request->result.value.message_page);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_GET:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_message_get(
        client, a, b, &request->result.value.message);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_AUDIO_DOWNLOAD:
    rc = (h2_pal_result_t)
        h2_gizclaw_client_friend_group_message_audio_download(
            client, a, b, social_write, request,
            &request->result.value.message_audio);
    break;
  }
  request->cancel_token = NULL;
  return rc;
}

static void social_complete(
    void *user, h2_gizclaw_operation_t *operation,
    const h2_gizclaw_operation_result_t *operation_result) {
  (void)operation;
  h2_gizclaw_social_request_t *request = user;
  h2_gizclaw_operation_result_t result = *operation_result;
  if (result.result != H2_PAL_OK)
    social_result_clear(request);
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->completion_user, request, &result,
                      result.result == H2_PAL_OK ? &request->result : NULL);
}

static void social_free(h2_gizclaw_social_request_t *request) {
  if (request == NULL)
    return;
  for (size_t index = 0u; index < 3u; ++index)
    h2_pal_mem_free(request->allocator, request->text[index]);
  h2_pal_mem_free(request->allocator, request);
}

static h2_pal_result_t social_submit(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_social_request_kind_t kind, h2_gizclaw_str_t a,
    h2_gizclaw_str_t b, h2_gizclaw_str_t c, size_t limit,
    h2_gizclaw_friend_group_role_t role,
    h2_gizclaw_friend_group_message_audio_write_fn write, void *write_user,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || completion == NULL || out_request == NULL ||
      !social_text_valid(a, false) || !social_text_valid(b, false) ||
      !social_text_valid(c, false))
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_social_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->allocator = allocator;
  request->kind = kind;
  request->result.kind = kind;
  request->completion = completion;
  request->completion_user = user;
  request->limit = limit;
  request->role = role;
  request->write = write;
  request->write_user = write_user;
  const h2_gizclaw_str_t values[3] = {a, b, c};
  for (size_t index = 0u; index < 3u; ++index) {
    request->text[index] = social_copy(allocator, values[index]);
    if (request->text[index] == NULL) {
      social_free(request);
      return H2_PAL_ERR_NO_MEMORY;
    }
  }
  const h2_pal_result_t rc = h2_gizclaw_service_submit(
      service, identity, social_run, social_complete, request,
      &request->operation);
  if (rc != H2_PAL_OK) {
    social_free(request);
    return rc;
  }
  *out_request = request;
  return H2_PAL_OK;
}

#define SOCIAL_SUBMIT0(kind)                                                   \
  return social_submit(service, identity, kind, (h2_gizclaw_str_t){0},        \
                       (h2_gizclaw_str_t){0}, (h2_gizclaw_str_t){0}, 0u,      \
                       H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, NULL, NULL,   \
                       completion, user, out_request)
#define SOCIAL_SUBMIT1(kind, a)                                                \
  return social_submit(service, identity, kind, a, (h2_gizclaw_str_t){0},     \
                       (h2_gizclaw_str_t){0}, 0u,                              \
                       H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, NULL, NULL,   \
                       completion, user, out_request)
#define SOCIAL_SUBMIT2(kind, a, b)                                             \
  return social_submit(service, identity, kind, a, b,                          \
                       (h2_gizclaw_str_t){0}, 0u,                              \
                       H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, NULL, NULL,   \
                       completion, user, out_request)
#define SOCIAL_SUBMIT3(kind, a, b, c)                                          \
  return social_submit(service, identity, kind, a, b, c, 0u,                  \
                       H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, NULL, NULL,   \
                       completion, user, out_request)

h2_pal_result_t h2_gizclaw_service_contacts_list_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  if (limit == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  return social_submit(service, identity, H2_GIZCLAW_SOCIAL_CONTACTS_LIST,
                       cursor, (h2_gizclaw_str_t){0},
                       (h2_gizclaw_str_t){0}, limit,
                       H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, NULL, NULL,
                       completion, user, out_request);
}

h2_pal_result_t h2_gizclaw_service_contact_get_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_CONTACT_GET, name);
}

h2_pal_result_t h2_gizclaw_service_contact_create_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t phone_number,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT3(H2_GIZCLAW_SOCIAL_CONTACT_CREATE, name, display_name,
                 phone_number);
}

h2_pal_result_t h2_gizclaw_service_contact_put_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t phone_number,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT3(H2_GIZCLAW_SOCIAL_CONTACT_PUT, name, display_name,
                 phone_number);
}

h2_pal_result_t h2_gizclaw_service_contact_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_CONTACT_DELETE, name);
}

static h2_pal_result_t social_list_submit(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_social_request_kind_t kind, h2_gizclaw_str_t first,
    h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  if (limit == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  return social_submit(service, identity, kind, first, cursor,
                       (h2_gizclaw_str_t){0}, limit,
                       H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, NULL, NULL,
                       completion, user, out_request);
}

h2_pal_result_t h2_gizclaw_service_friend_groups_list_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  return social_list_submit(service, identity,
                            H2_GIZCLAW_SOCIAL_FRIEND_GROUPS_LIST,
                            (h2_gizclaw_str_t){0}, cursor, limit, completion,
                            user, out_request);
}

h2_pal_result_t h2_gizclaw_service_friends_list_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  return social_list_submit(service, identity, H2_GIZCLAW_SOCIAL_FRIENDS_LIST,
                            (h2_gizclaw_str_t){0}, cursor, limit, completion,
                            user, out_request);
}

h2_pal_result_t h2_gizclaw_service_friend_info_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_id, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_INFO_GET, friend_id);
}
h2_pal_result_t h2_gizclaw_service_friend_add_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t invite_token, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_ADD, invite_token);
}
h2_pal_result_t h2_gizclaw_service_friend_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_id, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_DELETE, friend_id);
}
h2_pal_result_t h2_gizclaw_service_friend_invite_token_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT0(H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_GET);
}
h2_pal_result_t h2_gizclaw_service_friend_invite_token_create_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT0(H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CREATE);
}
h2_pal_result_t h2_gizclaw_service_friend_invite_token_clear_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT0(H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CLEAR);
}
h2_pal_result_t h2_gizclaw_service_friend_group_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_GET, group_name);
}
h2_pal_result_t h2_gizclaw_service_friend_group_create_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT3(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_CREATE, name, display_name,
                 description);
}
h2_pal_result_t h2_gizclaw_service_friend_group_put_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT3(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_PUT, name, display_name,
                 description);
}
h2_pal_result_t h2_gizclaw_service_friend_group_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_DELETE, group_name);
}
h2_pal_result_t h2_gizclaw_service_friend_group_join_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t invite_token, h2_gizclaw_str_t name,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT2(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_JOIN, invite_token, name);
}
h2_pal_result_t h2_gizclaw_service_friend_group_invite_token_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_GET, group_name);
}
h2_pal_result_t h2_gizclaw_service_friend_group_invite_token_create_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CREATE,
                 group_name);
}
h2_pal_result_t h2_gizclaw_service_friend_group_invite_token_clear_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CLEAR,
                 group_name);
}
h2_pal_result_t h2_gizclaw_service_friend_group_members_list_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  return social_list_submit(service, identity,
                            H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBERS_LIST,
                            group_name, cursor, limit, completion, user,
                            out_request);
}
h2_pal_result_t h2_gizclaw_service_friend_group_member_put_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t member_id,
    h2_gizclaw_friend_group_role_t role,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  return social_submit(service, identity,
                       H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_PUT, group_name,
                       member_id, (h2_gizclaw_str_t){0}, 0u, role, NULL, NULL,
                       completion, user, out_request);
}
h2_pal_result_t h2_gizclaw_service_friend_group_member_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t member_id,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT2(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_DELETE, group_name,
                 member_id);
}
h2_pal_result_t h2_gizclaw_service_friend_group_messages_list_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  return social_list_submit(service, identity,
                            H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGES_LIST,
                            group_name, cursor, limit, completion, user,
                            out_request);
}
h2_pal_result_t h2_gizclaw_service_friend_group_message_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t history_id,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT2(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_GET, group_name,
                 history_id);
}
h2_pal_result_t
h2_gizclaw_service_friend_group_message_audio_download_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_group_name, h2_gizclaw_str_t history_id,
    h2_gizclaw_friend_group_message_audio_write_fn write, void *write_user,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  if (write == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return social_submit(
      service, identity,
      H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_AUDIO_DOWNLOAD,
      friend_group_name, history_id, (h2_gizclaw_str_t){0}, 0u,
      H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, write, write_user, completion,
      user, out_request);
}

h2_pal_result_t h2_gizclaw_social_request_cancel(
    h2_gizclaw_social_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_operation_cancel(request->operation);
}

void h2_gizclaw_social_request_release(h2_gizclaw_social_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_operation_release(request->operation);
  social_result_clear(request);
  social_free(request);
}

#undef SOCIAL_SUBMIT0
#undef SOCIAL_SUBMIT1
#undef SOCIAL_SUBMIT2
#undef SOCIAL_SUBMIT3
