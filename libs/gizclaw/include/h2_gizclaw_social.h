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
  /** NULL after a successful get when no active token exists. */
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

h2_pal_result_t h2_gizclaw_req_create_friend_group_member_list(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_member_list(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_page_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_friend_group_member_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_page_t *out_result);

h2_pal_result_t h2_gizclaw_req_create_friend_group_member_put(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t member_id,
    h2_gizclaw_friend_group_role_t role, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_member_put(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_friend_group_member_put(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t member_id, h2_gizclaw_friend_group_role_t role,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_t *out_result);

h2_pal_result_t h2_gizclaw_req_create_friend_group_member_delete(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t member_id,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_member_delete(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_friend_group_member_delete(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t member_id, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_t *out_result);

h2_pal_result_t h2_gizclaw_req_create_friend_group_message_list(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_message_list(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_page_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_friend_group_message_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_page_t *out_result);

h2_pal_result_t h2_gizclaw_req_create_friend_group_message_get(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t history_id,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_message_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_friend_group_message_get(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t history_id, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_t *out_result);

/* create */
h2_pal_result_t h2_gizclaw_req_create_contact_list(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_contact_get(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_contact_create(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t phone_number,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_contact_put(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t phone_number,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_contact_delete(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_group_list(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

/* parse */
h2_pal_result_t
h2_gizclaw_resp_parse_contact_list(const h2_gizclaw_req_t *request,
                                   h2_gizclaw_resp_storage_t *storage,
                                   h2_gizclaw_contact_page_t *out_result);

h2_pal_result_t
h2_gizclaw_resp_parse_contact_get(const h2_gizclaw_req_t *request,
                                  h2_gizclaw_resp_storage_t *storage,
                                  h2_gizclaw_contact_t *out_result);

h2_pal_result_t
h2_gizclaw_resp_parse_contact_create(const h2_gizclaw_req_t *request,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_contact_t *out_result);

h2_pal_result_t
h2_gizclaw_resp_parse_contact_put(const h2_gizclaw_req_t *request,
                                  h2_gizclaw_resp_storage_t *storage,
                                  h2_gizclaw_contact_t *out_result);

h2_pal_result_t
h2_gizclaw_resp_parse_contact_delete(const h2_gizclaw_req_t *request,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_contact_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_friend_group_list(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_page_t *out_result);

/* sync */
h2_pal_result_t h2_gizclaw_rpc_contact_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_contact_page_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_contact_get(h2_gizclaw_service_t *service,
                                           h2_gizclaw_str_t name,
                                           uint32_t timeout_ms,
                                           h2_gizclaw_resp_storage_t *storage,
                                           h2_gizclaw_contact_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_contact_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t phone_number,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_contact_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_contact_put(h2_gizclaw_service_t *service,
                                           h2_gizclaw_str_t name,
                                           h2_gizclaw_str_t display_name,
                                           h2_gizclaw_str_t phone_number,
                                           uint32_t timeout_ms,
                                           h2_gizclaw_resp_storage_t *storage,
                                           h2_gizclaw_contact_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_contact_delete(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_contact_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_friend_group_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_page_t *out_result);

/* create */
h2_pal_result_t h2_gizclaw_req_create_friend_list(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_info_get(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_id, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_add(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t invite_token, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_delete(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_id, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_invite_token_get(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_invite_token_create(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_invite_token_clear(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

/* parse */
h2_pal_result_t
h2_gizclaw_resp_parse_friend_list(const h2_gizclaw_req_t *request,
                                  h2_gizclaw_resp_storage_t *storage,
                                  h2_gizclaw_friend_page_t *out_result);

h2_pal_result_t
h2_gizclaw_resp_parse_friend_info_get(const h2_gizclaw_req_t *request,
                                      h2_gizclaw_resp_storage_t *storage,
                                      h2_gizclaw_friend_t *out_result);

h2_pal_result_t
h2_gizclaw_resp_parse_friend_add(const h2_gizclaw_req_t *request,
                                 h2_gizclaw_resp_storage_t *storage,
                                 h2_gizclaw_friend_t *out_result);

h2_pal_result_t
h2_gizclaw_resp_parse_friend_delete(const h2_gizclaw_req_t *request,
                                    h2_gizclaw_resp_storage_t *storage,
                                    h2_gizclaw_friend_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_friend_invite_token_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_friend_invite_token_create(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_friend_invite_token_clear(
    const h2_gizclaw_req_t *request);

/* sync */
h2_pal_result_t h2_gizclaw_rpc_friend_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_page_t *out_result);

h2_pal_result_t
h2_gizclaw_rpc_friend_info_get(h2_gizclaw_service_t *service,
                               h2_gizclaw_str_t friend_id, uint32_t timeout_ms,
                               h2_gizclaw_resp_storage_t *storage,
                               h2_gizclaw_friend_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_friend_add(h2_gizclaw_service_t *service,
                                          h2_gizclaw_str_t invite_token,
                                          uint32_t timeout_ms,
                                          h2_gizclaw_resp_storage_t *storage,
                                          h2_gizclaw_friend_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_friend_delete(h2_gizclaw_service_t *service,
                                             h2_gizclaw_str_t friend_id,
                                             uint32_t timeout_ms,
                                             h2_gizclaw_resp_storage_t *storage,
                                             h2_gizclaw_friend_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_friend_invite_token_get(
    h2_gizclaw_service_t *service, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_invite_token_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_friend_invite_token_create(
    h2_gizclaw_service_t *service, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_invite_token_t *out_result);

h2_pal_result_t
h2_gizclaw_rpc_friend_invite_token_clear(h2_gizclaw_service_t *service,
                                         uint32_t timeout_ms);

/* create */
h2_pal_result_t h2_gizclaw_req_create_friend_group_get(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_group_create(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_group_put(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_group_delete(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_group_join(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t invite_token, h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_group_invite_token_get(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_group_invite_token_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_friend_group_invite_token_clear(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

/* parse */
h2_pal_result_t
h2_gizclaw_resp_parse_friend_group_get(const h2_gizclaw_req_t *request,
                                       h2_gizclaw_resp_storage_t *storage,
                                       h2_gizclaw_friend_group_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_friend_group_create(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_t *out_result);

h2_pal_result_t
h2_gizclaw_resp_parse_friend_group_put(const h2_gizclaw_req_t *request,
                                       h2_gizclaw_resp_storage_t *storage,
                                       h2_gizclaw_friend_group_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_friend_group_delete(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_t *out_result);

h2_pal_result_t
h2_gizclaw_resp_parse_friend_group_join(const h2_gizclaw_req_t *request,
                                        h2_gizclaw_resp_storage_t *storage,
                                        h2_gizclaw_friend_group_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_friend_group_invite_token_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_friend_group_invite_token_create(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_friend_group_invite_token_clear(
    const h2_gizclaw_req_t *request);

/* sync */
h2_pal_result_t h2_gizclaw_rpc_friend_group_get(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_friend_group_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_friend_group_put(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_friend_group_delete(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_friend_group_join(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t invite_token,
    h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_friend_group_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_friend_group_invite_token_get(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_friend_group_invite_token_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out_result);

h2_pal_result_t
h2_gizclaw_rpc_friend_group_invite_token_clear(h2_gizclaw_service_t *service,
                                               h2_gizclaw_str_t group_name,
                                               uint32_t timeout_ms);

/** Create a group-audio data-down request. Supply its writer to req_do. */
h2_pal_result_t h2_gizclaw_req_create_friend_group_message_audio_download(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t history_id,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_message_audio_download(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_audio_info_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_friend_group_message_audio_download(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t history_id,
    h2_gizclaw_friend_group_message_audio_write_fn write, void *write_user,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_audio_info_t *out_result);
#ifdef __cplusplus
}
#endif
#endif
