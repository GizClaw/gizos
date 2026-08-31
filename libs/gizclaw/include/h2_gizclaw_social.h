#ifndef H2_GIZCLAW_SOCIAL_H
#define H2_GIZCLAW_SOCIAL_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_service.h"
#include "h2_gizclaw_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_CONTACT_NAME_MAX_BYTES 255u
#define H2_GIZCLAW_CONTACT_DISPLAY_NAME_MAX_BYTES 255u
#define H2_GIZCLAW_CONTACT_PHONE_NUMBER_MAX_BYTES 64u
#define H2_GIZCLAW_CONTACT_TIMESTAMP_MAX_BYTES 64u
#define H2_GIZCLAW_CONTACT_CURSOR_MAX_BYTES 255u
#define H2_GIZCLAW_CONTACT_PAGE_MAX_ITEMS 64u
#define H2_GIZCLAW_FRIEND_GROUP_NAME_MAX_BYTES 255u
#define H2_GIZCLAW_FRIEND_GROUP_DISPLAY_NAME_MAX_BYTES 255u
#define H2_GIZCLAW_FRIEND_GROUP_DESCRIPTION_MAX_BYTES 1024u

/** Owned Contact snapshot returned by a Contact operation. */
typedef struct h2_gizclaw_contact {
  char *name;
  char *display_name;
  char *phone_number;
  char *created_at;
  char *updated_at;
} h2_gizclaw_contact_t;

/** Owned page returned by server.contact.list. */
typedef struct h2_gizclaw_contact_page {
  h2_gizclaw_contact_t *items;
  size_t count;
  bool has_next;
  char *next_cursor;
} h2_gizclaw_contact_page_t;

#if defined(H2_GIZCLAW_TESTING) || defined(H2_GIZCLAW_INTERNAL_SYNC_API)
/**
 * List Contacts visible to the authenticated Peer.
 *
 * This blocking unary RPC must run on the GizClaw client owner task. `cursor`
 * is borrowed for the call and `limit` must be positive. On success `out_page`
 * owns every item and text field; release them with
 * h2_gizclaw_contact_page_deinit().
 */
int h2_gizclaw_client_contacts_list(h2_gizclaw_client_t *client,
                                    h2_gizclaw_str_t cursor, size_t limit,
                                    h2_gizclaw_contact_page_t *out_page);

/**
 * Get one Contact by its caller-scoped immutable resource name.
 *
 * `name` is a borrowed non-empty UTF-8 span. On success `out_contact` owns its
 * fields; release them with h2_gizclaw_contact_deinit().
 */
int h2_gizclaw_client_contact_get(h2_gizclaw_client_t *client,
                                  h2_gizclaw_str_t name,
                                  h2_gizclaw_contact_t *out_contact);

/**
 * Create one Contact and return the server-owned stable snapshot.
 *
 * `name` is required. Display name and phone number are borrowed optional
 * UTF-8 strings. On success `out_contact` owns its fields; release them with
 * h2_gizclaw_contact_deinit().
 */
int h2_gizclaw_client_contact_create(h2_gizclaw_client_t *client,
                                     h2_gizclaw_str_t name,
                                     h2_gizclaw_str_t display_name,
                                     h2_gizclaw_str_t phone_number,
                                     h2_gizclaw_contact_t *out_contact);

/**
 * Update one Contact by immutable name and return the server-owned snapshot.
 *
 * `name` is required. Display name and phone number are borrowed optional
 * UTF-8 strings. On success `out_contact` owns its fields; release them with
 * h2_gizclaw_contact_deinit().
 */
int h2_gizclaw_client_contact_put(h2_gizclaw_client_t *client,
                                  h2_gizclaw_str_t name,
                                  h2_gizclaw_str_t display_name,
                                  h2_gizclaw_str_t phone_number,
                                  h2_gizclaw_contact_t *out_contact);

/**
 * Delete one Contact by immutable name and return the deleted server snapshot.
 *
 * `name` is a borrowed non-empty UTF-8 span. On success `out_contact` owns its
 * fields; release them with h2_gizclaw_contact_deinit().
 */
int h2_gizclaw_client_contact_delete(h2_gizclaw_client_t *client,
                                     h2_gizclaw_str_t name,
                                     h2_gizclaw_contact_t *out_contact);

/** Release storage owned by one Contact snapshot. */
void h2_gizclaw_contact_deinit(h2_gizclaw_client_t *client,
                               h2_gizclaw_contact_t *contact);

/** Release all storage owned by a Contact page. */
void h2_gizclaw_contact_page_deinit(h2_gizclaw_client_t *client,
                                    h2_gizclaw_contact_page_t *page);
#endif

/** Caller role returned with a FriendGroup snapshot. */
typedef enum h2_gizclaw_friend_group_role {
  H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED = 0,
  H2_GIZCLAW_FRIEND_GROUP_ROLE_OWNER,
  H2_GIZCLAW_FRIEND_GROUP_ROLE_ADMIN,
  H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER,
} h2_gizclaw_friend_group_role_t;

/** Owned FriendGroup snapshot returned by a Social operation. */
typedef struct h2_gizclaw_friend_group {
  char *name;
  char *display_name;
  char *description;
  char *workspace_name;
  h2_gizclaw_friend_group_role_t my_role;
} h2_gizclaw_friend_group_t;

/** Owned page returned by server.friend_group.list. */
typedef struct h2_gizclaw_friend_group_page {
  h2_gizclaw_friend_group_t *items;
  size_t count;
  bool has_next;
  char *next_cursor;
} h2_gizclaw_friend_group_page_t;

#if defined(H2_GIZCLAW_TESTING) || defined(H2_GIZCLAW_INTERNAL_SYNC_API)
/**
 * List FriendGroups visible to the authenticated Peer.
 *
 * This is a blocking unary RPC and must run on the GizClaw client owner task.
 * `cursor` is borrowed for the duration of the call; an empty cursor requests
 * the first page. `limit` must be positive. On success `out_page` owns its
 * items and text fields; release them with
 * h2_gizclaw_friend_group_page_deinit().
 */
int h2_gizclaw_client_friend_groups_list(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_friend_group_page_t *out_page);

/** Release all storage owned by a FriendGroup page. */
void h2_gizclaw_friend_group_page_deinit(h2_gizclaw_client_t *client,
                                         h2_gizclaw_friend_group_page_t *page);
#endif

/** Owned Friend relationship and its optional projected profile information. */
typedef struct h2_gizclaw_friend {
  /** Relationship ID copied verbatim from the wire FriendObject.name. */
  char *id;
  char *peer_public_key;
  char *workspace_name;
  char *created_at;
  char *updated_at;
  /** Optional projected profile display name. */
  char *name;
  char *emoji;
} h2_gizclaw_friend_t;

typedef struct h2_gizclaw_friend_page {
  h2_gizclaw_friend_t *items;
  size_t count;
  bool has_next;
  char *next_cursor;
} h2_gizclaw_friend_page_t;

/** Owned short-lived invitation returned by a Social token operation. */
typedef struct h2_gizclaw_invite_token {
  char *value;
  char *expires_at;
} h2_gizclaw_invite_token_t;

typedef struct h2_gizclaw_friend_group_member {
  /** Membership ID copied verbatim from the wire member name. */
  char *id;
  char *friend_group_name;
  char *peer_public_key;
  char *created_at;
  char *updated_at;
  h2_gizclaw_friend_group_role_t role;
} h2_gizclaw_friend_group_member_t;

typedef struct h2_gizclaw_friend_group_member_page {
  h2_gizclaw_friend_group_member_t *items;
  size_t count;
  bool has_next;
  char *next_cursor;
} h2_gizclaw_friend_group_member_page_t;

typedef enum h2_gizclaw_friend_group_message_type {
  H2_GIZCLAW_FRIEND_GROUP_MESSAGE_TYPE_UNSPECIFIED = 0,
  H2_GIZCLAW_FRIEND_GROUP_MESSAGE_TYPE_GEAR,
  H2_GIZCLAW_FRIEND_GROUP_MESSAGE_TYPE_AGENT,
} h2_gizclaw_friend_group_message_type_t;

typedef struct h2_gizclaw_friend_group_message {
  /** History ID copied verbatim from the wire message name. */
  char *history_id;
  char *friend_group_name;
  char *sender_peer_public_key;
  char *created_at;
  char *expires_at;
  char *name;
  char *text;
  h2_gizclaw_friend_group_message_type_t type;
  bool audio_available;
} h2_gizclaw_friend_group_message_t;

typedef struct h2_gizclaw_friend_group_message_audio_info {
  char *friend_group_name;
  char *history_id;
  char *mime_type;
  uint64_t size_bytes;
  uint64_t received_bytes;
} h2_gizclaw_friend_group_message_audio_info_t;

typedef int (*h2_gizclaw_friend_group_message_audio_write_fn)(
    void *user, const uint8_t *data, size_t len);

typedef struct h2_gizclaw_friend_group_message_page {
  h2_gizclaw_friend_group_message_t *items;
  size_t count;
  bool has_next;
  char *next_cursor;
} h2_gizclaw_friend_group_message_page_t;

typedef struct h2_gizclaw_social_request h2_gizclaw_social_request_t;

typedef enum h2_gizclaw_social_request_kind {
  H2_GIZCLAW_SOCIAL_CONTACTS_LIST = 0,
  H2_GIZCLAW_SOCIAL_CONTACT_GET,
  H2_GIZCLAW_SOCIAL_CONTACT_CREATE,
  H2_GIZCLAW_SOCIAL_CONTACT_PUT,
  H2_GIZCLAW_SOCIAL_CONTACT_DELETE,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUPS_LIST,
  H2_GIZCLAW_SOCIAL_FRIENDS_LIST,
  H2_GIZCLAW_SOCIAL_FRIEND_INFO_GET,
  H2_GIZCLAW_SOCIAL_FRIEND_ADD,
  H2_GIZCLAW_SOCIAL_FRIEND_DELETE,
  H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_GET,
  H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CREATE,
  H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CLEAR,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_GET,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_CREATE,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_PUT,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_DELETE,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_JOIN,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_GET,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CREATE,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CLEAR,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBERS_LIST,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_PUT,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_DELETE,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGES_LIST,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_GET,
  H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_AUDIO_DOWNLOAD,
} h2_gizclaw_social_request_kind_t;

typedef struct h2_gizclaw_social_result {
  h2_gizclaw_social_request_kind_t kind;
  union {
    h2_gizclaw_contact_page_t contact_page;
    h2_gizclaw_contact_t contact;
    h2_gizclaw_friend_group_page_t friend_group_page;
    h2_gizclaw_friend_page_t friend_page;
    h2_gizclaw_friend_t friend_value;
    h2_gizclaw_invite_token_t invite_token;
    h2_gizclaw_friend_group_t friend_group;
    h2_gizclaw_friend_group_member_page_t member_page;
    h2_gizclaw_friend_group_member_t member;
    h2_gizclaw_friend_group_message_page_t message_page;
    h2_gizclaw_friend_group_message_t message;
    h2_gizclaw_friend_group_message_audio_info_t message_audio;
  } value;
} h2_gizclaw_social_result_t;

typedef void (*h2_gizclaw_social_completion_fn)(
    void *user, h2_gizclaw_social_request_t *request,
    const h2_gizclaw_operation_result_t *operation_result,
    const h2_gizclaw_social_result_t *result);

h2_pal_result_t h2_gizclaw_service_contacts_list_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_contact_get_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_contact_create_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t phone_number,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_contact_put_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t phone_number,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_contact_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_groups_list_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friends_list_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_info_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_id, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_add_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t invite_token, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_id, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_invite_token_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_invite_token_create_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_invite_token_clear_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_create_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_put_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_join_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t invite_token, h2_gizclaw_str_t name,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_invite_token_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_invite_token_create_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_invite_token_clear_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_members_list_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_member_put_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t member_id,
    h2_gizclaw_friend_group_role_t role,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_member_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t member_id,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_messages_list_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_message_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t history_id,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_friend_group_message_audio_download_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_group_name, h2_gizclaw_str_t history_id,
    h2_gizclaw_friend_group_message_audio_write_fn write, void *write_user,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request);
h2_pal_result_t
h2_gizclaw_social_request_cancel(h2_gizclaw_social_request_t *request);
void h2_gizclaw_social_request_release(h2_gizclaw_social_request_t *request);

#if defined(H2_GIZCLAW_TESTING) || defined(H2_GIZCLAW_INTERNAL_SYNC_API)
int h2_gizclaw_client_friends_list(h2_gizclaw_client_t *client,
                                   h2_gizclaw_str_t cursor, size_t limit,
                                   h2_gizclaw_friend_page_t *out_page);
int h2_gizclaw_client_friend_info_get(h2_gizclaw_client_t *client,
                                      h2_gizclaw_str_t friend_id,
                                      h2_gizclaw_friend_t *out_friend);
int h2_gizclaw_client_friend_add(h2_gizclaw_client_t *client,
                                 h2_gizclaw_str_t invite_token,
                                 h2_gizclaw_friend_t *out_friend);
int h2_gizclaw_client_friend_delete(h2_gizclaw_client_t *client,
                                    h2_gizclaw_str_t friend_id,
                                    h2_gizclaw_friend_t *out_friend);
int h2_gizclaw_client_friend_invite_token_get(
    h2_gizclaw_client_t *client, h2_gizclaw_invite_token_t *out_token);
int h2_gizclaw_client_friend_invite_token_create(
    h2_gizclaw_client_t *client, h2_gizclaw_invite_token_t *out_token);
int h2_gizclaw_client_friend_invite_token_clear(h2_gizclaw_client_t *client);

int h2_gizclaw_client_friend_group_get(h2_gizclaw_client_t *client,
                                       h2_gizclaw_str_t group_name,
                                       h2_gizclaw_friend_group_t *out_group);
int h2_gizclaw_client_friend_group_create(h2_gizclaw_client_t *client,
                                          h2_gizclaw_str_t name,
                                          h2_gizclaw_str_t display_name,
                                          h2_gizclaw_str_t description,
                                          h2_gizclaw_friend_group_t *out_group);
int h2_gizclaw_client_friend_group_put(h2_gizclaw_client_t *client,
                                       h2_gizclaw_str_t name,
                                       h2_gizclaw_str_t display_name,
                                       h2_gizclaw_str_t description,
                                       h2_gizclaw_friend_group_t *out_group);
int h2_gizclaw_client_friend_group_delete(h2_gizclaw_client_t *client,
                                          h2_gizclaw_str_t group_name,
                                          h2_gizclaw_friend_group_t *out_group);
int h2_gizclaw_client_friend_group_join(h2_gizclaw_client_t *client,
                                        h2_gizclaw_str_t invite_token,
                                        h2_gizclaw_str_t name,
                                        h2_gizclaw_friend_group_t *out_group);
int h2_gizclaw_client_friend_group_invite_token_get(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t group_name,
    h2_gizclaw_invite_token_t *out_token);
int h2_gizclaw_client_friend_group_invite_token_create(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t group_name,
    h2_gizclaw_invite_token_t *out_token);
int h2_gizclaw_client_friend_group_invite_token_clear(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t group_name);

int h2_gizclaw_client_friend_group_members_list(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_friend_group_member_page_t *out_page);
int h2_gizclaw_client_friend_group_member_put(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t member_id, h2_gizclaw_friend_group_role_t role,
    h2_gizclaw_friend_group_member_t *out_member);
int h2_gizclaw_client_friend_group_member_delete(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t member_id, h2_gizclaw_friend_group_member_t *out_member);

int h2_gizclaw_client_friend_group_messages_list(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_friend_group_message_page_t *out_page);
int h2_gizclaw_client_friend_group_message_get(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t history_id,
    h2_gizclaw_friend_group_message_t *out_message);
int h2_gizclaw_client_friend_group_message_audio_download(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t friend_group_name,
    h2_gizclaw_str_t history_id,
    h2_gizclaw_friend_group_message_audio_write_fn write, void *user,
    h2_gizclaw_friend_group_message_audio_info_t *out_info);

void h2_gizclaw_friend_deinit(h2_gizclaw_client_t *client,
                              h2_gizclaw_friend_t *friend_value);
void h2_gizclaw_friend_page_deinit(h2_gizclaw_client_t *client,
                                   h2_gizclaw_friend_page_t *page);
void h2_gizclaw_invite_token_deinit(h2_gizclaw_client_t *client,
                                    h2_gizclaw_invite_token_t *token);
void h2_gizclaw_friend_group_deinit(h2_gizclaw_client_t *client,
                                    h2_gizclaw_friend_group_t *group);
void h2_gizclaw_friend_group_member_deinit(
    h2_gizclaw_client_t *client, h2_gizclaw_friend_group_member_t *member);
void h2_gizclaw_friend_group_member_page_deinit(
    h2_gizclaw_client_t *client, h2_gizclaw_friend_group_member_page_t *page);
void h2_gizclaw_friend_group_message_deinit(
    h2_gizclaw_client_t *client, h2_gizclaw_friend_group_message_t *message);
void h2_gizclaw_friend_group_message_page_deinit(
    h2_gizclaw_client_t *client, h2_gizclaw_friend_group_message_page_t *page);
void h2_gizclaw_friend_group_message_audio_info_deinit(
    h2_gizclaw_client_t *client,
    h2_gizclaw_friend_group_message_audio_info_t *info);
#endif

#ifdef __cplusplus
}
#endif

#endif
