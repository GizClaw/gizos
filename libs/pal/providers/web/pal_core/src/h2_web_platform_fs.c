#include "h2_web_fs.h"
#include "h2_posix_pal_core.h"
#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_WEB_FS_DEFAULT_LOCK_TIMEOUT_MS 3000u
#define H2_WEB_FS_COMMIT_TIMEOUT_MS 30000u

EM_JS_DEPS(h2_web_fs, "$FS,$IDBFS");

typedef struct h2_web_fs_waiter {
  struct h2_web_fs_waiter *next;
  h2_web_async_t op;
} h2_web_fs_waiter_t;

struct h2_web_fs {
  h2_pal_fs_api_t api;
  h2_web_platform_t *platform;
  h2_posix_host_fs_t *host;
  const h2_pal_fs_api_t *inner;
  char *root;
  size_t root_len;
  char **readonly;
  size_t readonly_count;
  uint64_t changed;
  uint64_t committed;
  uint64_t failed;
  uint64_t syncing_generation;
  h2_pal_result_t failed_result;
  bool syncing;
  bool mounted;
  h2_web_fs_waiter_t *waiters;
  size_t open_files;
  unsigned calls;
};

typedef struct h2_web_fs_file {
  h2_pal_fs_file_t *inner;
  bool persistent_write;
} h2_web_fs_file_t;

// clang-format off
EM_JS(int, h2_web_fs_mount_js,
      (uintptr_t platform_address, uintptr_t fs_address, uint32_t op_id,
       const char *root_ptr, uint32_t lock_timeout_ms), {
  const root = UTF8ToString(root_ptr);
  const entries = Module['h2WebFs'] ||= new Map();
  const entry = {root, mount: null, release: null, error: ""};
  const classify = (error) => {
    const cause = error?.target?.error || error?.error || error;
    const name = cause?.name || "";
    const message = `${name || 'Error'}: ${cause?.message || cause}`;
    let code = -4;
    if (name === 'QuotaExceededError' || /quota/i.test(message)) code = -13;
    else if (/indexedDB not supported|not available/i.test(message)) code = -3;
    else if (['SecurityError', 'InvalidStateError', 'NotAllowedError',
              'VersionError'].includes(name)) code = -2;
    return [code, message];
  };
  entry.classify = classify;
  let settled = false;
  const finish = (code, message) => {
    if (settled) return;
    settled = true;
    entry.error = message || "";
    Module['h2WebFsLastError'] = code ? {root, code, message} : null;
    if (code) {
      if (entry.mount) {
        try { FS.unmount(root); } catch (_) {}
        entry.mount = null;
      }
      entry.release?.();
      entry.release = null;
    }
    Module['_h2_web_async_complete'](platform_address, op_id, code);
  };
  if (typeof indexedDB === 'undefined' || !indexedDB) {
    entries.set(fs_address, entry);
    finish(-3, 'IndexedDB is unavailable in this context');
    return 0;
  }
  const locks = globalThis.navigator?.locks;
  if (!locks || typeof locks.request !== 'function') {
    entries.set(fs_address, entry);
    finish(-3, 'Web Locks are unavailable; serve the page over HTTPS or ' +
               'localhost so one tab owns the storage');
    return 0;
  }
  entries.set(fs_address, entry);
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), lock_timeout_ms);
  const held = new Promise((resolve) => { entry.release = resolve; });
  locks.request(`h2-web-fs:${root}`, {signal: controller.signal}, () => {
    clearTimeout(timer);
    if (settled || entries.get(fs_address) !== entry) return;
    try {
      try { FS.mkdirTree(root); } catch (_) {}
      const node = FS.lookupPath(root).node;
      if (FS.isMountpoint(node)) {
        finish(-18, `${root} is already mounted in this page`);
        return held;
      }
      entry.mount = FS.mount(IDBFS, {}, root).mount;
    } catch (error) {
      finish(-4, `mount ${root} failed: ${error?.message || error}`);
      return held;
    }
    IDBFS.syncfs(entry.mount, true, (error) => {
      if (!error) {
        finish(0, "");
        return;
      }
      const [code, message] = classify(error);
      finish(code, `restore ${root} failed: ${message}`);
    });
    return held;
  }).catch((error) => {
    clearTimeout(timer);
    if (error?.name === 'AbortError') {
      finish(-18, `${root} is in use by another tab or window of this ` +
                  'origin; close it or wait');
    } else {
      finish(-2, `Web Lock for ${root} failed: ${error?.name}: ` +
                 `${error?.message || error}`);
    }
  });
  return 0;
});

EM_JS(int, h2_web_fs_sync_js, (uintptr_t fs_address, double generation), {
  const entries = Module['h2WebFs'];
  const entry = entries?.get(fs_address);
  if (!entry?.mount) return -7;
  IDBFS.syncfs(entry.mount, false, (error) => {
    if (entries.get(fs_address) !== entry) return;
    let code = 0;
    if (error) {
      const [classified, message] = entry.classify(error);
      code = classified;
      entry.error = `commit ${entry.root} failed: ${message}`;
      console.error(`Web FS ${entry.error}`);
    } else {
      entry.error = "";
    }
    Module['_h2_web_fs_synced'](fs_address, generation, code);
  });
  return 0;
});

EM_JS(void, h2_web_fs_error_js,
      (uintptr_t fs_address, char *out, size_t out_size), {
  const message = Module['h2WebFs']?.get(fs_address)?.error || "";
  stringToUTF8(message, out, out_size);
});

EM_JS(void, h2_web_fs_unmount_js, (uintptr_t fs_address), {
  const entries = Module['h2WebFs'];
  const entry = entries?.get(fs_address);
  if (!entry) return;
  entries.delete(fs_address);
  if (entry.mount) {
    try { FS.unmount(entry.root); } catch (_) {}
  }
  entry.release?.();
});
// clang-format on

static void h2_web_fs_log(const char *message) {
  (void)h2_pal_log_write(h2_web_platform_log_api(), H2_PAL_LOG_ERROR,
                         "web_fs", message);
}

static void h2_web_fs_wake_waiters(h2_web_fs_t *fs) {
  for (h2_web_fs_waiter_t *waiter = fs->waiters; waiter != NULL;
       waiter = waiter->next)
    h2_web_async_signal(fs->platform, &waiter->op, H2_PAL_OK);
}

static void h2_web_fs_start_sync(h2_web_fs_t *fs) {
  if (fs->syncing || !fs->mounted)
    return;
  fs->syncing = true;
  fs->syncing_generation = fs->changed;
  const int result =
      h2_web_fs_sync_js((uintptr_t)fs, (double)fs->syncing_generation);
  if (result != H2_PAL_OK) {
    fs->syncing = false;
    fs->failed = fs->syncing_generation;
    fs->failed_result = (h2_pal_result_t)result;
    h2_web_fs_wake_waiters(fs);
  }
}

EMSCRIPTEN_KEEPALIVE void h2_web_fs_synced(uintptr_t fs_address,
                                           double generation, int result) {
  h2_web_fs_t *fs = (h2_web_fs_t *)fs_address;
  if (fs == NULL || !fs->syncing)
    return;
  fs->syncing = false;
  const uint64_t committed = (uint64_t)generation;
  if (result == H2_PAL_OK) {
    if (committed > fs->committed)
      fs->committed = committed;
    fs->failed_result = H2_PAL_OK;
  } else {
    fs->failed = committed;
    fs->failed_result = (h2_pal_result_t)result;
  }
  // Changes made while this commit ran need another one for their waiters.
  if (fs->waiters != NULL && fs->changed > fs->committed &&
      fs->changed > fs->failed)
    h2_web_fs_start_sync(fs);
  h2_web_fs_wake_waiters(fs);
}

/*
 * Return after IndexedDB committed every change made before this call. Each
 * call starts at most one commit of its own, so a failure is retried by the
 * next barrier rather than cached forever.
 */
static h2_pal_result_t h2_web_fs_commit(h2_web_fs_t *fs) {
  const uint64_t target = fs->changed;
  const double deadline_ms =
      emscripten_get_now() + (double)H2_WEB_FS_COMMIT_TIMEOUT_MS;
  bool attempted = false;
  for (;;) {
    if (fs->committed >= target)
      return H2_PAL_OK;
    if (!fs->syncing) {
      if (attempted)
        return fs->failed_result != H2_PAL_OK ? fs->failed_result
                                              : H2_PAL_ERR_IO;
      h2_web_fs_start_sync(fs);
      attempted = true;
      if (!fs->syncing)
        return fs->failed_result;
    }
    const double remaining_ms = deadline_ms - emscripten_get_now();
    if (remaining_ms <= 0.0)
      return H2_PAL_ERR_TIMEOUT;
    h2_web_fs_waiter_t waiter;
    h2_web_async_begin(fs->platform, &waiter.op);
    waiter.next = fs->waiters;
    fs->waiters = &waiter;
    const h2_pal_result_t result = h2_web_async_wait(
        fs->platform, &waiter.op, (uint32_t)remaining_ms + 1u);
    h2_web_fs_waiter_t **cursor = &fs->waiters;
    while (*cursor != &waiter)
      cursor = &(*cursor)->next;
    *cursor = waiter.next;
    h2_web_async_end(fs->platform, &waiter.op);
    if (result == H2_PAL_ERR_CLOSED)
      return H2_PAL_ERR_CLOSED;
  }
}

static bool h2_web_fs_under(const char *root, size_t root_len,
                            const char *path) {
  return path != NULL && strncmp(path, root, root_len) == 0 &&
         (path[root_len] == '\0' || path[root_len] == '/');
}

static bool h2_web_fs_persistent(const h2_web_fs_t *fs, const char *path) {
  return h2_web_fs_under(fs->root, fs->root_len, path);
}

static bool h2_web_fs_readonly(const h2_web_fs_t *fs, const char *path) {
  for (size_t index = 0u; index < fs->readonly_count; ++index)
    if (h2_web_fs_under(fs->readonly[index], strlen(fs->readonly[index]),
                        path))
      return true;
  return false;
}

/*
 * Run one metadata mutation; persistent changes return after commit. A
 * failed mutation may still have changed memory (a partial clear), so it is
 * counted and the next barrier commits whatever did change.
 */
static int h2_web_fs_mutated(h2_web_fs_t *fs, const char *path, int result) {
  if (!h2_web_fs_persistent(fs, path))
    return result;
  ++fs->changed;
  if (result != H2_PAL_OK)
    return result;
  return h2_web_fs_commit(fs);
}

static int h2_web_fs_mkdir(void *user, const char *path) {
  h2_web_fs_t *fs = user;
  if (h2_web_fs_readonly(fs, path))
    return H2_PAL_ERR_UNSUPPORTED;
  ++fs->calls;
  const int result =
      h2_web_fs_mutated(fs, path, h2_pal_fs_mkdir(fs->inner, path));
  --fs->calls;
  return result;
}

static int h2_web_fs_open_file(void *user, const char *path,
                               h2_pal_fs_open_mode_t mode,
                               h2_pal_fs_file_t **out_file) {
  h2_web_fs_t *fs = user;
  *out_file = NULL;
  const bool writing = mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE;
  if (writing && h2_web_fs_readonly(fs, path))
    return H2_PAL_ERR_UNSUPPORTED;
  h2_web_fs_file_t *file = calloc(1u, sizeof(*file));
  if (file == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  const int result = h2_pal_fs_open(fs->inner, path, mode, &file->inner);
  if (result != H2_PAL_OK) {
    free(file);
    return result;
  }
  file->persistent_write = writing && h2_web_fs_persistent(fs, path);
  if (file->persistent_write)
    ++fs->changed;
  ++fs->open_files;
  *out_file = (h2_pal_fs_file_t *)file;
  return H2_PAL_OK;
}

static int h2_web_fs_read(void *user, h2_pal_fs_file_t *raw, void *data,
                          size_t len, size_t *out_read) {
  h2_web_fs_t *fs = user;
  h2_web_fs_file_t *file = (h2_web_fs_file_t *)raw;
  return h2_pal_fs_read(fs->inner, file->inner, data, len, out_read);
}

static int h2_web_fs_seek(void *user, h2_pal_fs_file_t *raw,
                          uint64_t position) {
  h2_web_fs_t *fs = user;
  h2_web_fs_file_t *file = (h2_web_fs_file_t *)raw;
  return h2_pal_fs_seek(fs->inner, file->inner, position);
}

static int h2_web_fs_write(void *user, h2_pal_fs_file_t *raw, const void *data,
                           size_t len, size_t *out_written) {
  h2_web_fs_t *fs = user;
  h2_web_fs_file_t *file = (h2_web_fs_file_t *)raw;
  const int result =
      h2_pal_fs_write(fs->inner, file->inner, data, len, out_written);
  if (file->persistent_write && *out_written != 0u)
    ++fs->changed;
  return result;
}

static int h2_web_fs_sync(void *user, h2_pal_fs_file_t *raw) {
  h2_web_fs_t *fs = user;
  h2_web_fs_file_t *file = (h2_web_fs_file_t *)raw;
  int result = h2_pal_fs_sync(fs->inner, file->inner);
  if (result == H2_PAL_OK && file->persistent_write) {
    ++fs->calls;
    result = h2_web_fs_commit(fs);
    --fs->calls;
  }
  return result;
}

static int h2_web_fs_close_file(void *user, h2_pal_fs_file_t *raw) {
  h2_web_fs_t *fs = user;
  h2_web_fs_file_t *file = (h2_web_fs_file_t *)raw;
  int result = h2_pal_fs_close(fs->inner, file->inner);
  const bool commit = file->persistent_write;
  free(file);
  --fs->open_files;
  if (commit) {
    ++fs->changed;
    ++fs->calls;
    const int commit_result = h2_web_fs_commit(fs);
    --fs->calls;
    if (result == H2_PAL_OK)
      result = commit_result;
  }
  return result;
}

static int h2_web_fs_stat(void *user, const char *path,
                          h2_pal_fs_stat_t *out_stat) {
  h2_web_fs_t *fs = user;
  return h2_pal_fs_stat(fs->inner, path, out_stat);
}

static int h2_web_fs_clear_path(void *user, const char *path) {
  h2_web_fs_t *fs = user;
  if (h2_web_fs_readonly(fs, path))
    return H2_PAL_ERR_UNSUPPORTED;
  ++fs->calls;
  const int result =
      h2_web_fs_mutated(fs, path, h2_pal_fs_clear(fs->inner, path));
  --fs->calls;
  return result;
}

static int h2_web_fs_remove(void *user, const char *path) {
  h2_web_fs_t *fs = user;
  if (h2_web_fs_readonly(fs, path))
    return H2_PAL_ERR_UNSUPPORTED;
  ++fs->calls;
  const int result =
      h2_web_fs_mutated(fs, path, h2_pal_fs_remove(fs->inner, path));
  --fs->calls;
  return result;
}

static int h2_web_fs_rename(void *user, const char *old_path,
                            const char *new_path) {
  h2_web_fs_t *fs = user;
  if (h2_web_fs_readonly(fs, old_path) || h2_web_fs_readonly(fs, new_path))
    return H2_PAL_ERR_UNSUPPORTED;
  ++fs->calls;
  const int result = h2_web_fs_mutated(
      fs, new_path, h2_pal_fs_rename(fs->inner, old_path, new_path));
  --fs->calls;
  return result;
}

static const h2_pal_fs_vtable_t h2_web_fs_vtable = {
    .mkdir = h2_web_fs_mkdir,
    .open = h2_web_fs_open_file,
    .read = h2_web_fs_read,
    .seek = h2_web_fs_seek,
    .write = h2_web_fs_write,
    .sync = h2_web_fs_sync,
    .close = h2_web_fs_close_file,
    .stat = h2_web_fs_stat,
    .clear = h2_web_fs_clear_path,
    .remove = h2_web_fs_remove,
    .rename = h2_web_fs_rename,
};

static bool h2_web_fs_root_valid(const char *path) {
  if (path == NULL || path[0] != '/' || path[1] == '\0')
    return false;
  const size_t len = strlen(path);
  if (path[len - 1u] == '/' || strstr(path, "//") != NULL ||
      strstr(path, "/./") != NULL || strstr(path, "/../") != NULL)
    return false;
  const char *last = strrchr(path, '/');
  return strcmp(last, "/.") != 0 && strcmp(last, "/..") != 0;
}

static bool h2_web_fs_overlap(const char *left, const char *right) {
  const size_t left_len = strlen(left);
  const size_t right_len = strlen(right);
  return left_len <= right_len ? h2_web_fs_under(left, left_len, right)
                               : h2_web_fs_under(right, right_len, left);
}

static void h2_web_fs_free(h2_web_fs_t *fs) {
  if (fs->host != NULL)
    h2_posix_host_fs_destroy(fs->host);
  for (size_t index = 0u; index < fs->readonly_count; ++index)
    free(fs->readonly[index]);
  free(fs->readonly);
  free(fs->root);
  free(fs);
}

h2_pal_result_t h2_web_fs_open(h2_web_platform_t *platform,
                               const h2_web_fs_config_t *config,
                               h2_web_fs_t **out_fs) {
  if (out_fs != NULL)
    *out_fs = NULL;
  if (platform == NULL || config == NULL || out_fs == NULL ||
      platform->shutting_down || !h2_web_fs_root_valid(config->persistent_root) ||
      (config->readonly_root_count != 0u && config->readonly_roots == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t index = 0u; index < config->readonly_root_count; ++index) {
    const char *root = config->readonly_roots[index];
    if (!h2_web_fs_root_valid(root) ||
        h2_web_fs_overlap(root, config->persistent_root))
      return H2_PAL_ERR_INVALID_ARG;
    for (size_t other = 0u; other < index; ++other)
      if (h2_web_fs_overlap(root, config->readonly_roots[other]))
        return H2_PAL_ERR_INVALID_ARG;
  }
  h2_web_fs_t *fs = calloc(1u, sizeof(*fs));
  if (fs == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  fs->platform = platform;
  fs->api = (h2_pal_fs_api_t){.user = fs, .vtable = &h2_web_fs_vtable};
  fs->root = strdup(config->persistent_root);
  fs->readonly = config->readonly_root_count == 0u
                     ? NULL
                     : calloc(config->readonly_root_count, sizeof(char *));
  bool allocated = fs->root != NULL &&
                   (config->readonly_root_count == 0u || fs->readonly != NULL);
  for (size_t index = 0u; allocated && index < config->readonly_root_count;
       ++index) {
    fs->readonly[index] = strdup(config->readonly_roots[index]);
    allocated = fs->readonly[index] != NULL;
    fs->readonly_count = index + 1u;
  }
  if (!allocated) {
    h2_web_fs_free(fs);
    return H2_PAL_ERR_NO_MEMORY;
  }
  fs->root_len = strlen(fs->root);

  h2_web_async_t op;
  h2_web_async_begin(platform, &op);
  int result = h2_web_fs_mount_js(
      (uintptr_t)platform, (uintptr_t)fs, op.id, fs->root,
      config->lock_timeout_ms == 0u ? H2_WEB_FS_DEFAULT_LOCK_TIMEOUT_MS
                                    : config->lock_timeout_ms);
  result = h2_web_async_finish(platform, &op, result);
  if (result != H2_PAL_OK) {
    char message[H2_WEB_FS_ERROR_MAX];
    h2_web_fs_error_js((uintptr_t)fs, message, sizeof(message));
    char line[H2_WEB_FS_ERROR_MAX + 48u];
    (void)snprintf(line, sizeof(line), "open rc=%d %s", result, message);
    h2_web_fs_log(line);
    h2_web_fs_unmount_js((uintptr_t)fs);
    h2_web_fs_free(fs);
    return (h2_pal_result_t)result;
  }
  fs->mounted = true;

  const size_t mount_count = 1u + fs->readonly_count;
  const char **roots = calloc(mount_count, sizeof(*roots));
  if (roots == NULL) {
    h2_web_fs_unmount_js((uintptr_t)fs);
    h2_web_fs_free(fs);
    return H2_PAL_ERR_NO_MEMORY;
  }
  roots[0] = fs->root;
  for (size_t index = 0u; index < fs->readonly_count; ++index)
    roots[index + 1u] = fs->readonly[index];
  result = h2_posix_host_fs_create(roots, roots, mount_count, &fs->host);
  free(roots);
  if (result != H2_PAL_OK) {
    h2_web_fs_log("open failed: a read-only root is missing from the "
                  "Emscripten filesystem");
    h2_web_fs_unmount_js((uintptr_t)fs);
    h2_web_fs_free(fs);
    return (h2_pal_result_t)result;
  }
  fs->inner = h2_posix_host_fs_api(fs->host);
  ++platform->open_filesystems;
  *out_fs = fs;
  return H2_PAL_OK;
}

const h2_pal_fs_api_t *h2_web_fs_api(h2_web_fs_t *fs) {
  return fs == NULL ? NULL : &fs->api;
}

h2_pal_result_t h2_web_fs_flush(h2_web_fs_t *fs) {
  if (fs == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  ++fs->calls;
  const h2_pal_result_t result = h2_web_fs_commit(fs);
  --fs->calls;
  return result;
}

h2_pal_result_t h2_web_fs_clear(h2_web_fs_t *fs) {
  if (fs == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return (h2_pal_result_t)h2_web_fs_clear_path(fs, fs->root);
}

h2_pal_result_t h2_web_fs_get_status(h2_web_fs_t *fs,
                                     h2_web_fs_status_t *out_status) {
  if (fs == NULL || out_status == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_status, 0, sizeof(*out_status));
  out_status->changed_generation = fs->changed;
  out_status->committed_generation = fs->committed;
  out_status->last_result = fs->failed_result;
  if (fs->failed_result != H2_PAL_OK)
    h2_web_fs_error_js((uintptr_t)fs, out_status->last_error,
                       sizeof(out_status->last_error));
  return H2_PAL_OK;
}

h2_pal_result_t h2_web_fs_close(h2_web_fs_t *fs) {
  if (fs == NULL)
    return H2_PAL_OK;
  if (fs->open_files != 0u || fs->calls != 0u)
    return H2_PAL_ERR_BUSY;
  ++fs->calls;
  h2_pal_result_t result = h2_web_fs_commit(fs);
  // A commit already in flight still reports into this object; let it land,
  // but no longer than one commit timeout.
  const double deadline_ms =
      emscripten_get_now() + (double)H2_WEB_FS_COMMIT_TIMEOUT_MS;
  while (fs->syncing && emscripten_get_now() < deadline_ms &&
         h2_web_platform_sleep_ms(fs->platform, 1u) == H2_PAL_OK) {
  }
  --fs->calls;
  if (fs->syncing)
    return H2_PAL_ERR_TIMEOUT;
  h2_web_fs_unmount_js((uintptr_t)fs);
  --fs->platform->open_filesystems;
  h2_web_fs_free(fs);
  return result;
}
