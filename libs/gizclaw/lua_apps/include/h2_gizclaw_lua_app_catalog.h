#ifndef H2_GIZCLAW_LUA_APP_CATALOG_H
#define H2_GIZCLAW_LUA_APP_CATALOG_H

#include "h2/pal/application/h2_pal_http.h"
#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_LUA_APPS_MAX 32u
#define H2_GIZCLAW_LUA_APP_PATH_MAX 192u
#define H2_GIZCLAW_LUA_APP_FILES_MAX 64u
#define H2_GIZCLAW_LUA_APP_URL_MAX 1024u

typedef struct h2_gizclaw_lua_app_catalog h2_gizclaw_lua_app_catalog_t;
typedef struct h2_gizclaw_lua_app_install h2_gizclaw_lua_app_install_t;
typedef struct h2_gizclaw_lua_app_lease h2_gizclaw_lua_app_lease_t;

/** Borrowed HTTP(S) URL bytes; no NUL termination is required. Addresses are
 * at most H2_GIZCLAW_LUA_APP_URL_MAX bytes. Other schemes are rejected. */
typedef struct h2_gizclaw_lua_app_source {
  const char *data;
  size_t size;
} h2_gizclaw_lua_app_source_t;

/** Caller-owned snapshot. entry and data_dir are resolved filesystem paths;
 * user storage is independent of both. All strings are NUL terminated. */
typedef struct h2_gizclaw_lua_app_info {
  char app_id[33];
  char version[32];
  char display_name[129];
  char description[1025];
  char entry[H2_GIZCLAW_LUA_APP_PATH_MAX];
  char data_dir[H2_GIZCLAW_LUA_APP_PATH_MAX];
} h2_gizclaw_lua_app_info_t;

/** Dependencies are borrowed until destroy. Paths are copied at create.
 * preinstalled_index is optional: format=1, apps=[{root,manifest}], where each
 * manifest is the public lua_app format-1 manifest. Missing index means no
 * preinstalled apps. Missing/oversized entries are omitted from discovery.
 * install_root is optional for read-only discovery. Never place mutable app
 * storage under it: installation and cleanup own its complete contents. */
typedef struct h2_gizclaw_lua_app_catalog_config {
  const h2_pal_mem_api_t *allocator;
  const h2_pal_fs_api_t *fs;
  const h2_pal_sync_api_t *sync;
  const char *preinstalled_index;
  const char *install_root;
  /** Zero selects 256 KiB Lua text, 2 MiB per package, 8 MiB installed total,
   * 32 apps and 64 payload files. Limits include preinstalled identities;
   * installed byte accounting excludes the immutable preinstalled files. */
  size_t source_limit_bytes;
  uint64_t app_limit_bytes;
  uint64_t installed_limit_bytes;
  size_t max_apps;
  size_t max_files;
} h2_gizclaw_lua_app_catalog_config_t;

/** Loads both indexes without networking or starting tasks. An installed
 * app shadows its preinstalled identity. Corrupt indexes fail closed. Failure
 * normally leaves out_apps NULL. An ambiguous filesystem close failure leaves
 * a non-NULL retained handle: keep it and all dependencies alive for backend
 * or process recovery, and do not use it except to inspect destroy's error.
 * A rollback mutex-destroy failure also retains a non-NULL handle; retry
 * checked destroy before releasing dependencies. No other operation is valid
 * on a catalog whose creation failed. */
h2_pal_result_t h2_gizclaw_lua_app_catalog_create(
    const h2_gizclaw_lua_app_catalog_config_t *config,
    h2_gizclaw_lua_app_catalog_t **out_apps);
/** No operation may be in progress. BUSY leaves the instance alive. A prior
 * filesystem close error also retains the instance and returns that error:
 * PAL cannot prove whether retrying close would access a consumed handle.
 * This terminal storage failure requires backend/process recovery. */
h2_pal_result_t
h2_gizclaw_lua_app_catalog_destroy(h2_gizclaw_lua_app_catalog_t *apps);
/** Retain the catalog for a consumer such as a browser bridge or device.
 * Does not pin any identity. Concurrent installation can return BUSY; failure
 * acquires no reference. Destroy returns BUSY until this reference is released.
 * Creation/destruction remain serial with attempts to acquire new consumers. */
h2_pal_result_t h2_gizclaw_lua_app_catalog_retain(
    h2_gizclaw_lua_app_catalog_t *apps);
/** Release one consumer reference, clear its borrowed pointer, and never
 * access files or wait for an installer. NULL is a no-op. This does not
 * destroy the owner's catalog; use destroy after the last consumer closes. */
void h2_gizclaw_lua_app_catalog_release(h2_gizclaw_lua_app_catalog_t **apps);
/** Non-blocking serialized snapshots; concurrent operations return BUSY.
 * A NULL output with capacity zero reports the required count. */
h2_pal_result_t
h2_gizclaw_lua_app_catalog_list(h2_gizclaw_lua_app_catalog_t *apps,
                                h2_gizclaw_lua_app_info_t *out, size_t capacity,
                                size_t *out_count);
h2_pal_result_t
h2_gizclaw_lua_app_catalog_get(h2_gizclaw_lua_app_catalog_t *apps,
                               const char *app_id,
                               h2_gizclaw_lua_app_info_t *out);
/** Atomically resolve an identity and pin its entry/data paths. Installation
 * and uninstall of that identity return BUSY until every lease reference is
 * released. Other identities and snapshots remain available. The product
 * retains the lease until every Lua worker using these paths has joined.
 * A lease also prevents catalog destruction. No long-lived mutex is held.
 */
h2_pal_result_t h2_gizclaw_lua_app_catalog_acquire(
    h2_gizclaw_lua_app_catalog_t *apps, const char *app_id,
    h2_gizclaw_lua_app_info_t *out, h2_gizclaw_lua_app_lease_t **out_lease);
/** Acquire another owned reference to a live lease. */
h2_pal_result_t
h2_gizclaw_lua_app_lease_retain(h2_gizclaw_lua_app_lease_t *lease);
/** Nonblocking release, including while another identity is being installed.
 * NULL is a no-op. Clears the caller's pointer and never accesses files.
 */
void h2_gizclaw_lua_app_lease_release(h2_gizclaw_lua_app_lease_t **lease);

/** True when an installation root and the required filesystem operations are
 * configured. Does not probe volume permissions, health or available room. */
bool h2_gizclaw_lua_app_catalog_can_install(
    const h2_gizclaw_lua_app_catalog_t *apps);
bool h2_gizclaw_lua_app_id_valid(const char *app_id);

/** Idempotently uninstall an identity, preserving independent user storage.
 * Atomically remove it from discovery before reclaiming both writable slots.
 * Preinstalled identities are persistently hidden; immutable firmware files
 * remain intact. Installing the identity again enables it. Requires the same
 * filesystem operations as installation; concurrent operations return BUSY.
 * Index failure preserves the app. Cleanup failure can follow publication:
 * the app remains unavailable; repeat uninstall to retry cleanup. Never retry
 * an ambiguous close failure; keep dependencies alive as documented above.
 * Acquire a catalog lease for every running user of entry/data_dir snapshots.
 */
h2_pal_result_t
h2_gizclaw_lua_app_catalog_uninstall(h2_gizclaw_lua_app_catalog_t *apps,
                                     const char *app_id);

/** Open an incremental zlib/USTAR installation. compressed_size is the exact
 * expected byte count, or zero when the transport does not declare it. SHA-256
 * is optional (NULL), otherwise exactly 64 hexadecimal characters. The handle
 * holds the directory's installation reservation until finish or abort; every
 * operation on it must run on the same task. Buffers are borrowed only during
 * write. These synchronous filesystem operations belong on a worker when the
 * transport requires prompt callbacks. No compressed archive is retained. */
h2_pal_result_t h2_gizclaw_lua_app_install_begin(
    h2_gizclaw_lua_app_catalog_t *apps, uint64_t compressed_size,
    const char *sha256, h2_pal_http_cancel_fn cancel, void *cancel_user,
    h2_gizclaw_lua_app_install_t **out_install);
h2_pal_result_t
h2_gizclaw_lua_app_install_write(h2_gizclaw_lua_app_install_t *install,
                                 const uint8_t *bytes, size_t size);
/** Consume the handle on success or failure. Success follows exact length,
 * digest, TAR/zlib EOS, manifest/file checks, and atomic index publication. */
h2_pal_result_t
h2_gizclaw_lua_app_install_finish(h2_gizclaw_lua_app_install_t **install,
                                  h2_gizclaw_lua_app_info_t *out);
/** Idempotently consume an unfinished handle, attempt staging cleanup and
 * release its reservation. Never removes the previously published app. An
 * ambiguous close error retains the manager/dependencies as documented above.
 */
void h2_gizclaw_lua_app_install_abort(h2_gizclaw_lua_app_install_t **install);

/** Synchronously downloads, inflates and verifies a complete format-1 USTAR
 * package through bounded buffers. No compressed archive is retained. The
 * optional SHA-256 covers compressed bytes. Supports only HTTP and HTTPS,
 * without credentials/fragments. Direct archive delivery uses the incremental
 * begin/write/finish API. No automatic retry. timeout_ms bounds HTTP and
 * must be 1..110000. Cancellation is also checked during extraction/commit.
 * Success means the new index was atomically published. Errors preserve the
 * previous app and all user data. Requires replacing atomic fs.rename.
 * No callbacks or HTTP views are retained after return. */
h2_pal_result_t h2_gizclaw_lua_app_catalog_install(
    h2_gizclaw_lua_app_catalog_t *apps, const h2_pal_http_api_t *http,
    h2_gizclaw_lua_app_source_t source, const char *sha256, uint32_t timeout_ms,
    h2_pal_http_cancel_fn cancel, void *cancel_user,
    h2_gizclaw_lua_app_info_t *out);

#ifdef __cplusplus
}
#endif
#endif
