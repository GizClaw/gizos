#ifndef H2_LUA_STORAGE_INTERNAL_H
#define H2_LUA_STORAGE_INTERNAL_H

#include "../runtime/h2_lua_internal.h"

#define STORAGE_READ_ATTEMPTS 4

typedef enum storage_status {
  STORAGE_OK = 0,
  STORAGE_UNAVAILABLE,
  STORAGE_INVALID_NAME,
  STORAGE_NOT_FOUND,
  STORAGE_QUOTA_EXCEEDED,
  STORAGE_TOO_MANY_FILES,
  STORAGE_NO_SPACE,
  STORAGE_CHANGED,
  STORAGE_BUSY,
  STORAGE_IO,
  STORAGE_CORRUPT,
} storage_status_t;

/* Host-scoped synchronization shared by storage and kv. _locked helpers
 * borrow caller buffers and require this mutex; none acquire it recursively,
 * allocate memory or call Lua. Buffers are valid only for the duration of
 * calls. */
int h2_lua_storage_available(const h2_lua_job_t *job);
storage_status_t h2_lua_storage_lock(h2_lua_job_t *job);
void h2_lua_storage_unlock(h2_lua_job_t *job);
storage_status_t h2_lua_storage_kv_size_locked(h2_lua_job_t *job,
                                               uint64_t *size);
storage_status_t h2_lua_storage_kv_read_locked(h2_lua_job_t *job, void *data,
                                               size_t size);
/* Zero size deletes the snapshot; nonzero size checks shared quotas and
 * replaces it. */
storage_status_t h2_lua_storage_kv_commit_locked(h2_lua_job_t *job,
                                                 const void *data, size_t size);

#endif
