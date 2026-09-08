#ifndef H2_GIZCLAW_RESOURCE_H
#define H2_GIZCLAW_RESOURCE_H

#include "h2_gizclaw_app_config.h"
#include "h2_gizclaw_points.h"
#include "h2_gizclaw_profile.h"
#include "h2_gizclaw_social.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One independently allocated, caller-scoped resource store. */
typedef struct h2_gizclaw_resource h2_gizclaw_resource_t;
typedef enum h2_gizclaw_resource_kind {
  H2_GIZCLAW_RESOURCE_CONTACTS = 0,
  H2_GIZCLAW_RESOURCE_PROFILE,
  H2_GIZCLAW_RESOURCE_POINTS,
  H2_GIZCLAW_RESOURCE_GROUPS,
  H2_GIZCLAW_RESOURCE_APP_CONFIG,
} h2_gizclaw_resource_kind_t;
typedef enum h2_gizclaw_resource_operation {
  H2_GIZCLAW_RESOURCE_REFRESH = 0,
  H2_GIZCLAW_RESOURCE_LOAD_MORE,
  H2_GIZCLAW_RESOURCE_CONTACT_CREATE,
  H2_GIZCLAW_RESOURCE_CONTACT_UPDATE,
  H2_GIZCLAW_RESOURCE_CONTACT_DELETE,
  H2_GIZCLAW_RESOURCE_PROFILE_NAME,
  H2_GIZCLAW_RESOURCE_PROFILE_EMOJI,
} h2_gizclaw_resource_operation_t;

typedef struct h2_gizclaw_resource_config {
  h2_gizclaw_resource_kind_t kind;
  h2_gizclaw_service_t *service;
  const h2_pal_mem_api_t *mem;
  const h2_pal_sync_api_t *sync;
  const h2_pal_time_api_t *time;
  h2_runtime_t *runtime;
  size_t max_items;
  size_t page_size;
  size_t storage_bytes;
} h2_gizclaw_resource_config_t;

/** Strings are borrowed only for the duration of execute. Contact names must
 * remain stable when retrying uncertain mutations. text is a profile name or
 * emoji for profile operations, and unused for contact operations. */
typedef struct h2_gizclaw_resource_command {
  h2_gizclaw_resource_operation_t operation;
  const char *name;
  const char *display_name;
  const char *phone_number;
  const char *text;
} h2_gizclaw_resource_command_t;

/** Pointer fields in the selected union member belong to the caller's response
 * storage. valid means a complete data snapshot exists; stale means its
 * freshness cannot be asserted (refreshing, failed operation or closed
 * connection). revision counts notifications, data_revision counts committed
 * snapshots. Points balance has independent validity/result from the
 * transaction list. These local revisions are not server revisions. */
typedef struct h2_gizclaw_resource_snapshot {
  h2_gizclaw_resource_kind_t kind;
  uint64_t revision;
  uint64_t data_revision;
  bool valid;
  bool stale;
  bool busy;
  bool closed;
  h2_pal_result_t last_error;
  bool balance_valid;
  h2_pal_result_t balance_result;
  union {
    h2_gizclaw_contact_page_t contacts;
    h2_gizclaw_profile_t profile;
    struct {
      h2_gizclaw_points_account_t account;
      h2_gizclaw_points_transaction_page_t transactions;
    } points;
    h2_gizclaw_friend_group_page_t groups;
    h2_gizclaw_app_config_snapshot_t app_config;
  } data;
} h2_gizclaw_resource_snapshot_t;

/** Borrows dependencies until destroy; allocates bounded snapshots lazily.
 * Use one store per kind on a Service, and route mutations through that store.
 * Persistence, offline admission policy and UI drafts belong to the consumer.
 */
h2_pal_result_t
h2_gizclaw_resource_create(const h2_gizclaw_resource_config_t *config,
                           h2_gizclaw_resource_t **out);
/** Join callers first. BUSY leaves the handle intact. Does not free Service. */
h2_pal_result_t h2_gizclaw_resource_destroy(h2_gizclaw_resource_t **resource);
/** Short locked read, never RPC. Deep-copy into storage, append at
 * storage.used. Failure clears out and rolls back storage. Stale data remains
 * readable and must not be treated as fresh. An empty valid list is distinct
 * from no data. */
h2_pal_result_t
h2_gizclaw_resource_snapshot(h2_gizclaw_resource_t *resource,
                             h2_gizclaw_resp_storage_t *storage,
                             h2_gizclaw_resource_snapshot_t *out);
/** Blocking caller-worker operation; never call from service_poll callbacks or
 * the Service network task. Concurrent execute returns BUSY. timeout_ms bounds
 * the entire operation including paging/reconciliation. Contact/group refresh
 * loads the complete bounded list. Points refresh replaces its first page;
 * LOAD_MORE appends using the owned cursor and requires a fresh valid list.
 * Mutations reload confirmed data; failures keep prior data marked stale.
 * AppConfig supports REFRESH only: loads every key/value at one Profile
 * name/revision, replacing the complete snapshot only on success.
 * Points may commit a successful balance or list independently.
 * No product defaults, filesystem access or optimistic updates are performed.
 */
h2_pal_result_t
h2_gizclaw_resource_execute(h2_gizclaw_resource_t *resource,
                            const h2_gizclaw_resource_command_t *command,
                            uint32_t timeout_ms);
/** Permanently closes admission and discards late results. Call before Service
 * stop, then join execute callers before destroy. Does not erase retained data
 * or roll back server-side mutations. Reconnect with a new resource instance.
 */
h2_pal_result_t h2_gizclaw_resource_close(h2_gizclaw_resource_t *resource);

#ifdef __cplusplus
}
#endif
#endif
