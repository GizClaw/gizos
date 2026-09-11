#ifndef H2_WEB_FS_H
#define H2_WEB_FS_H

#include "h2_web_platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Browser Filesystem PAL provider with one IndexedDB-backed persistent root.
 *
 * The provider exposes only the configured roots. Read-only roots are
 * directories the page already placed in the Emscripten filesystem (for
 * example --preload-file assets); every mutation below them returns
 * UNSUPPORTED. The persistent root is an Emscripten IDBFS mount whose
 * IndexedDB database is named by the root path within the page origin. Paths
 * outside the roots return NOT_FOUND.
 *
 * Durability: write() only changes memory. sync(), close() of a file opened
 * for writing, rename(), remove(), mkdir() and clear() below the persistent
 * root are commit barriers: they return after IndexedDB committed every
 * change made so far, or with the commit error (NO_SPACE for quota,
 * UNAVAILABLE when storage is blocked, IO otherwise, TIMEOUT after 30 s).
 * A failed commit leaves the change in memory; the next barrier retries it.
 * Barrier callers in a task yield to other tasks; root callers need Asyncify.
 * Committed data survives reload, tab close and browser restart. Browsers
 * commit IndexedDB with relaxed durability, and best-effort site storage can
 * be evicted under storage pressure or cleared by the user.
 *
 * Exclusivity: opening takes the Web Lock "h2-web-fs:<root>" for the life of
 * the provider, so one tab or worker of an origin owns a root at a time. Other
 * tabs wait up to lock_timeout_ms and then get BUSY; a reload waits for the
 * previous document to release the lock. Web Locks need a secure context
 * (HTTPS or localhost); without them open returns UNSUPPORTED rather than risk
 * two tabs overwriting each other's snapshot.
 */
typedef struct h2_web_fs h2_web_fs_t;

typedef struct h2_web_fs_config {
  /** Absolute writable directory, e.g. "/data/gizclaw"; created if absent. */
  const char *persistent_root;
  /** Absolute directories already present in the Emscripten filesystem. */
  const char *const *readonly_roots;
  size_t readonly_root_count;
  /** Wait for another tab to release the root; zero selects 3000 ms. */
  uint32_t lock_timeout_ms;
} h2_web_fs_config_t;

#define H2_WEB_FS_ERROR_MAX 192u

typedef struct h2_web_fs_status {
  /** Mutations made below the persistent root so far. */
  uint64_t changed_generation;
  /** Mutations committed to IndexedDB; equal when nothing is pending. */
  uint64_t committed_generation;
  /** OK, or the result of the most recent failed commit. */
  h2_pal_result_t last_result;
  /** Browser error name and message of that failure, empty after success. */
  char last_error[H2_WEB_FS_ERROR_MAX];
} h2_web_fs_status_t;

/**
 * Lock the persistent root, mount it and restore its IndexedDB contents.
 *
 * Call before the App starts so it sees the restored data. Errors: INVALID_ARG
 * for a relative, "/" or overlapping root or a missing read-only root; BUSY
 * when another tab holds the root; UNSUPPORTED without IndexedDB or Web Locks;
 * UNAVAILABLE when the browser blocks storage (e.g. some private modes);
 * NO_SPACE for quota; IO for other restore failures. Failures are logged with
 * the browser error.
 */
h2_pal_result_t h2_web_fs_open(h2_web_platform_t *platform,
                               const h2_web_fs_config_t *config,
                               h2_web_fs_t **out_fs);

/** Borrow the PAL Filesystem API; valid until h2_web_fs_close() succeeds. */
const h2_pal_fs_api_t *h2_web_fs_api(h2_web_fs_t *fs);

/** Commit every change made so far; OK when nothing is pending. */
h2_pal_result_t h2_web_fs_flush(h2_web_fs_t *fs);

/** Delete everything below the persistent root and commit the deletion. */
h2_pal_result_t h2_web_fs_clear(h2_web_fs_t *fs);

h2_pal_result_t h2_web_fs_get_status(h2_web_fs_t *fs,
                                     h2_web_fs_status_t *out_status);

/**
 * Commit pending changes, unmount the root and release its Web Lock.
 *
 * Returns BUSY, leaving the provider open, while files are open or calls are
 * in progress, and TIMEOUT, also leaving it open, when a commit the browser
 * started does not settle within 30 s. Otherwise the provider is released
 * even when the final commit fails; that commit error is returned. NULL
 * returns OK.
 */
h2_pal_result_t h2_web_fs_close(h2_web_fs_t *fs);

#ifdef __cplusplus
}
#endif

#endif
