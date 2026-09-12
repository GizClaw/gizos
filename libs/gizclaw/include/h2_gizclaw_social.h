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

/** server.friend_group.members.add (59) adds the Peer with public key text
 * peer_public_key (as in h2_gizclaw_friend_t.peer_public_key) to the caller's
 * FriendGroup group_name. member_name is the FriendGroup name the added Peer
 * sees in its own list; it follows the group_name rules. role is ADMIN or
 * MEMBER; the key is 1..64 printable ASCII bytes. Create copies every input
 * and performs no network I/O.
 *
 * The Server decides who may add and how many: ADMIN needs the caller to be
 * the owner, MEMBER an owner or admin, and a FriendGroup holds at most 10
 * members including the owner. It does not require a Friend relationship.
 * Re-adding a current member under the same member_name changes that
 * member's role; the owner cannot be re-added. A group the caller does not
 * belong to fails with H2_PAL_ERR_NOT_FOUND. A full group, a target already in
 * its maximum number of groups, a missing permission, a conflicting
 * member_name and every other Server rejection fail with
 * H2_GIZCLAW_ERR_REMOTE, as for the other Social wrappers. Parse decodes the
 * returned member like member_put and member_delete. */
h2_pal_result_t h2_gizclaw_req_create_friend_group_member_add(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t peer_public_key,
    h2_gizclaw_str_t member_name, h2_gizclaw_friend_group_role_t role,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_member_add(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_friend_group_member_add(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t peer_public_key, h2_gizclaw_str_t member_name,
    h2_gizclaw_friend_group_role_t role, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_t *out_result);

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

/** Outcome of server.friend.ping / server.friend_group.ping. */
typedef enum h2_gizclaw_social_ping_result {
  H2_GIZCLAW_SOCIAL_PING_RESULT_UNSPECIFIED = 0,
  /** At least one target device acknowledged the ping. */
  H2_GIZCLAW_SOCIAL_PING_RESULT_DELIVERED = 1,
  /** No target device is online; nothing was sent and no rate-limit window
   * was started. */
  H2_GIZCLAW_SOCIAL_PING_RESULT_NOT_ONLINE = 2,
  /** The friend pair or the FriendGroup pinged within the Server window. */
  H2_GIZCLAW_SOCIAL_PING_RESULT_RATE_LIMITED = 3,
} h2_gizclaw_social_ping_result_t;

/** Caller-owned ping outcome; needs no response storage.
 * delivered_count is at least 1 only for DELIVERED (a friend ping reaches at
 * most one device) and 0 otherwise. retry_after_seconds is present, and at
 * least 1, only for RATE_LIMITED. A response violating these rules, or with
 * an unknown result, fails to parse with H2_PAL_ERR_FORMAT. */
typedef struct h2_gizclaw_social_ping {
  h2_gizclaw_social_ping_result_t result;
  uint32_t delivered_count;
  bool has_retry_after_seconds;
  uint32_t retry_after_seconds;
} h2_gizclaw_social_ping_t;

/** server.friend.ping (123) pings the caller's Friend by relationship ID
 * (h2_gizclaw_friend_t.id); the Server pushes client.social.ping to that
 * device. Create copies friend_id and performs no network I/O. */
h2_pal_result_t h2_gizclaw_req_create_friend_ping(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_id, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_friend_ping(const h2_gizclaw_req_t *request,
                                  h2_gizclaw_social_ping_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_friend_ping(h2_gizclaw_service_t *service,
                                           h2_gizclaw_str_t friend_id,
                                           uint32_t timeout_ms,
                                           h2_gizclaw_social_ping_t *out_result);

/** server.friend_group.ping (124) rallies every other member device of the
 * caller's FriendGroup name. Same ownership and validation as friend_ping. */
h2_pal_result_t h2_gizclaw_req_create_friend_group_ping(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_friend_group_ping(const h2_gizclaw_req_t *request,
                                        h2_gizclaw_social_ping_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_friend_group_ping(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    uint32_t timeout_ms, h2_gizclaw_social_ping_t *out_result);

#define H2_GIZCLAW_PUBLIC_PROFILE_MAX_KEYS 16u
#define H2_GIZCLAW_PEER_PUBLIC_KEY_MAX_BYTES 64u

/** Owned public projection of one Peer's DeviceInfo. Only the self-chosen
 * display name and emoji are public; either is NULL when the Peer does not
 * exist or has not set it. */
typedef struct h2_gizclaw_public_profile {
  char *peer_public_key;
  char *display_name;
  char *emoji;
} h2_gizclaw_public_profile_t;

/** One item per distinct requested key, in request order. */
typedef struct h2_gizclaw_public_profile_list {
  h2_gizclaw_public_profile_t *items;
  size_t count;
} h2_gizclaw_public_profile_list_t;

/** server.profile.get (125) looks up 1..16 Peers by public key text (as in
 * h2_gizclaw_friend_t.peer_public_key). Each key must be non-empty, at most
 * 64 bytes and printable ASCII; the Server enforces the canonical encoding.
 * Duplicate keys are sent as given and answered once. Create copies every key
 * and performs no network I/O. Parse fails with H2_PAL_ERR_FORMAT unless the
 * items are exactly the distinct requested keys in request order. */
h2_pal_result_t h2_gizclaw_req_create_public_profile_get(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_str_t *peer_public_keys, size_t key_count,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_resp_parse_public_profile_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_public_profile_list_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_public_profile_get(
    h2_gizclaw_service_t *service, const h2_gizclaw_str_t *peer_public_keys,
    size_t key_count, uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_public_profile_list_t *out_result);
#ifdef __cplusplus
}
#endif
#endif
