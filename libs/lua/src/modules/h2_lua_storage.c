#include "../runtime/h2_lua_internal.h"

#include <string.h>

/* Per-app flat file store for the ESP-Claw `storage` module profile.
 *
 * Files live directly under `<root>/<app_id>/`. PAL Filesystem cannot list a
 * directory, so each app keeps a newline-separated `.index` of its names. A
 * name enters the index before its file is created and leaves it after the
 * file is removed, so an interrupted operation can only leave an index entry
 * without a file, which the next load prunes. Content and index are both
 * replaced through a temporary file and a replacing rename. Names never start
 * with '.', so the index and temporary files cannot collide with app files.
 *
 * Every operation runs on the owning worker under the Host storage mutex and
 * never calls Lua while holding it, so a Lua error cannot leak the lock or an
 * open file. */

#define STORAGE_INDEX_NAME ".index"
#define STORAGE_INDEX_TEMP_NAME ".index.tmp"
#define STORAGE_DATA_TEMP_NAME ".data.tmp"
#define STORAGE_DEFAULT_QUOTA_BYTES (64u * 1024u)
#define STORAGE_DEFAULT_MAX_FILES 16u
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
} storage_status_t;

typedef struct storage_entry {
  char name[H2_LUA_STORAGE_NAME_MAX + 1u];
  uint64_t size;
} storage_entry_t;

/* One index at a time, owned by whoever holds the storage mutex. Holds one
 * entry more than app_max_files so a rename can record its target before the
 * source leaves the index. */
struct h2_lua_storage_scratch {
  storage_entry_t *entries;
  size_t entry_capacity;
  size_t count;
  int dirty;
  char *text;
  size_t text_capacity;
};

typedef struct h2_lua_storage_scratch storage_scratch_t;

static const char *storage_message(storage_status_t status) {
  switch (status) {
  case STORAGE_UNAVAILABLE:
    return "storage: unavailable";
  case STORAGE_INVALID_NAME:
    return "storage: invalid name";
  case STORAGE_NOT_FOUND:
    return "storage: not found";
  case STORAGE_QUOTA_EXCEEDED:
    return "storage: quota exceeded";
  case STORAGE_TOO_MANY_FILES:
    return "storage: too many files";
  case STORAGE_NO_SPACE:
    return "storage: no space";
  case STORAGE_BUSY:
  case STORAGE_CHANGED:
    return "storage: busy";
  case STORAGE_OK:
  case STORAGE_IO:
  default:
    return "storage: io error";
  }
}

static storage_status_t from_pal(int result) {
  switch (result) {
  case H2_PAL_OK:
    return STORAGE_OK;
  case H2_PAL_ERR_NOT_FOUND:
    return STORAGE_NOT_FOUND;
  case H2_PAL_ERR_NO_SPACE:
    return STORAGE_NO_SPACE;
  default:
    return STORAGE_IO;
  }
}

int h2_lua_storage_name_is_valid(const char *name, size_t max_length) {
  size_t i;
  if (name == NULL || name[0] == '\0' || name[0] == '.') {
    return 0;
  }
  for (i = 0u; name[i] != '\0'; ++i) {
    char c = name[i];
    if (i >= max_length || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                             c == '_' || c == '-' || c == '.')) {
      return 0;
    }
  }
  return 1;
}

h2_pal_result_t h2_lua_storage_normalize(h2_lua_storage_config_t *config) {
  const h2_pal_fs_vtable_t *vtable;
  size_t root_length;
  if (config == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (config->fs == NULL) {
    return H2_PAL_OK;
  }
  vtable = config->fs->vtable;
  if (vtable == NULL || vtable->mkdir == NULL || vtable->open == NULL ||
      vtable->read == NULL || vtable->write == NULL || vtable->close == NULL ||
      vtable->stat == NULL || vtable->remove == NULL ||
      vtable->rename == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  if (config->root == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  root_length = strlen(config->root);
  if (root_length == 0u || root_length > H2_LUA_STORAGE_ROOT_MAX ||
      config->root[root_length - 1u] == '/') {
    return H2_PAL_ERR_INVALID_ARG;
  }
  config->app_quota_bytes = config->app_quota_bytes == 0u
                                ? STORAGE_DEFAULT_QUOTA_BYTES
                                : config->app_quota_bytes;
  config->app_max_files = config->app_max_files == 0u
                              ? STORAGE_DEFAULT_MAX_FILES
                              : config->app_max_files;
  if (config->app_max_files > H2_LUA_STORAGE_MAX_FILES) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_lua_storage_host_init(h2_lua_host_t *host) {
  const h2_lua_storage_config_t *config = &host->config.storage;
  const h2_pal_mem_api_t *mem = host->config.runtime->mem;
  storage_scratch_t *scratch;
  size_t entry_capacity;
  h2_pal_result_t result;
  if (config->fs == NULL) {
    return H2_PAL_OK;
  }
  memcpy(host->storage_root, config->root, strlen(config->root) + 1u);
  host->config.storage.root = host->storage_root;
  entry_capacity = config->app_max_files + 1u;
  scratch = h2_pal_mem_alloc(mem, sizeof(*scratch));
  if (scratch == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(scratch, 0, sizeof(*scratch));
  host->storage_scratch = scratch;
  scratch->entry_capacity = entry_capacity;
  scratch->text_capacity = entry_capacity * (H2_LUA_STORAGE_NAME_MAX + 1u);
  scratch->entries =
      h2_pal_mem_alloc(mem, entry_capacity * sizeof(*scratch->entries));
  scratch->text = h2_pal_mem_alloc(mem, scratch->text_capacity);
  if (scratch->entries == NULL || scratch->text == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  result = h2_pal_mutex_create(host->config.runtime->sync,
                               &(h2_pal_mutex_config_t){
                                   .name = "h2-lua-storage",
                                   .allocator = mem,
                               },
                               &host->storage_mutex);
  return result;
}

void h2_lua_storage_host_deinit(h2_lua_host_t *host) {
  const h2_pal_mem_api_t *mem = host->config.runtime->mem;
  if (host->storage_mutex != NULL) {
    (void)h2_pal_mutex_destroy(host->config.runtime->sync, host->storage_mutex);
    host->storage_mutex = NULL;
  }
  if (host->storage_scratch != NULL) {
    h2_pal_mem_free(mem, host->storage_scratch->entries);
    h2_pal_mem_free(mem, host->storage_scratch->text);
    h2_pal_mem_free(mem, host->storage_scratch);
    host->storage_scratch = NULL;
  }
}

static h2_lua_job_t *storage_job(lua_State *state) {
  return lua_touserdata(state, lua_upvalueindex(1));
}

static const h2_pal_fs_api_t *storage_fs(const h2_lua_job_t *job) {
  return job->host->config.storage.fs;
}

static int storage_available(const h2_lua_job_t *job) {
  return storage_fs(job) != NULL && job->host->storage_mutex != NULL &&
         job->app_id[0] != '\0';
}

/* Writes `<root>/<app_id>` or `<root>/<app_id>/<name>`. Bounded by the
 * root, app id and name limits, so it always fits H2_LUA_PATH_MAX. */
static void storage_path(const h2_lua_job_t *job, const char *name,
                         char *out_path) {
  size_t offset = strlen(job->host->storage_root);
  size_t length;
  memcpy(out_path, job->host->storage_root, offset);
  out_path[offset++] = '/';
  length = strlen(job->app_id);
  memcpy(out_path + offset, job->app_id, length);
  offset += length;
  if (name != NULL) {
    out_path[offset++] = '/';
    length = strlen(name);
    memcpy(out_path + offset, name, length);
    offset += length;
  }
  out_path[offset] = '\0';
}

static storage_status_t storage_lock(h2_lua_job_t *job) {
  return h2_pal_mutex_lock(job->host->config.runtime->sync,
                           job->host->storage_mutex) == H2_PAL_OK
             ? STORAGE_OK
             : STORAGE_BUSY;
}

static void storage_unlock(h2_lua_job_t *job) {
  (void)h2_pal_mutex_unlock(job->host->config.runtime->sync,
                            job->host->storage_mutex);
}

static long storage_find(const storage_scratch_t *scratch, const char *name) {
  size_t i;
  for (i = 0u; i < scratch->count; ++i) {
    if (strcmp(scratch->entries[i].name, name) == 0) {
      return (long)i;
    }
  }
  return -1;
}

static void storage_drop(storage_scratch_t *scratch, size_t index) {
  memmove(&scratch->entries[index], &scratch->entries[index + 1u],
          (scratch->count - index - 1u) * sizeof(scratch->entries[0]));
  scratch->count--;
}

static uint64_t storage_used(const storage_scratch_t *scratch) {
  uint64_t used = 0u;
  size_t i;
  for (i = 0u; i < scratch->count; ++i) {
    used += scratch->entries[i].size;
  }
  return used;
}

static storage_status_t storage_read_exact(const h2_pal_fs_api_t *fs,
                                           const char *path, void *data,
                                           size_t length, size_t *out_read,
                                           int allow_short) {
  h2_pal_fs_file_t *file = NULL;
  size_t offset = 0u;
  int result = h2_pal_fs_open(fs, path, H2_PAL_FS_OPEN_READ, &file);
  while (result == H2_PAL_OK && offset < length) {
    size_t read_size = 0u;
    result = h2_pal_fs_read(fs, file, (uint8_t *)data + offset, length - offset,
                            &read_size);
    if (result == H2_PAL_OK && read_size == 0u) {
      if (!allow_short) {
        result = H2_PAL_ERR_TRUNCATED;
      }
      break;
    }
    offset += read_size;
  }
  if (file != NULL) {
    int close_result = h2_pal_fs_close(fs, file);
    if (result == H2_PAL_OK) {
      result = close_result;
    }
  }
  *out_read = offset;
  return from_pal(result);
}

static storage_status_t storage_write_whole(const h2_pal_fs_api_t *fs,
                                            const char *path, const void *data,
                                            size_t length) {
  h2_pal_fs_file_t *file = NULL;
  size_t offset = 0u;
  int result = h2_pal_fs_open(fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
  while (result == H2_PAL_OK && offset < length) {
    size_t written = 0u;
    result = h2_pal_fs_write(fs, file, (const uint8_t *)data + offset,
                             length - offset, &written);
    if (result == H2_PAL_OK && written == 0u) {
      result = H2_PAL_ERR_IO;
    }
    offset += written;
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_fs_sync(fs, file);
  }
  if (file != NULL) {
    int close_result = h2_pal_fs_close(fs, file);
    if (result == H2_PAL_OK) {
      result = close_result;
    }
  }
  return from_pal(result);
}

/* Replaces `target_name` in the app directory as a whole, or leaves it
 * untouched on failure. */
static storage_status_t storage_replace(h2_lua_job_t *job,
                                        const char *temp_name,
                                        const char *target_name,
                                        const void *data, size_t length) {
  const h2_pal_fs_api_t *fs = storage_fs(job);
  char temp_path[H2_LUA_PATH_MAX];
  char target_path[H2_LUA_PATH_MAX];
  storage_status_t status;
  storage_path(job, temp_name, temp_path);
  storage_path(job, target_name, target_path);
  status = storage_write_whole(fs, temp_path, data, length);
  if (status == STORAGE_OK) {
    status = from_pal(h2_pal_fs_rename(fs, temp_path, target_path));
    /* A missing temp file here means the fs lost it, not a missing name. */
    if (status == STORAGE_NOT_FOUND) {
      status = STORAGE_IO;
    }
  }
  if (status != STORAGE_OK) {
    (void)h2_pal_fs_remove(fs, temp_path);
  }
  return status;
}

static storage_status_t storage_ensure_dir(h2_lua_job_t *job) {
  const h2_pal_fs_api_t *fs = storage_fs(job);
  char path[H2_LUA_PATH_MAX];
  h2_pal_fs_stat_t stat;
  size_t i;
  storage_path(job, NULL, path);
  /* Parents may be mount points that refuse mkdir; only the app directory
   * itself has to exist afterwards. */
  for (i = 1u; path[i] != '\0'; ++i) {
    if (path[i] == '/') {
      path[i] = '\0';
      (void)h2_pal_fs_mkdir(fs, path);
      path[i] = '/';
    }
  }
  (void)h2_pal_fs_mkdir(fs, path);
  if (h2_pal_fs_stat(fs, path, &stat) != H2_PAL_OK || !stat.is_dir) {
    return STORAGE_IO;
  }
  return STORAGE_OK;
}

static storage_status_t storage_save_index(h2_lua_job_t *job) {
  storage_scratch_t *scratch = job->host->storage_scratch;
  size_t offset = 0u;
  size_t i;
  storage_status_t status;
  for (i = 0u; i < scratch->count; ++i) {
    size_t length = strlen(scratch->entries[i].name);
    memcpy(scratch->text + offset, scratch->entries[i].name, length);
    offset += length;
    scratch->text[offset++] = '\n';
  }
  status = storage_replace(job, STORAGE_INDEX_TEMP_NAME, STORAGE_INDEX_NAME,
                           scratch->text, offset);
  if (status == STORAGE_OK) {
    scratch->dirty = 0;
  }
  return status;
}

/* Loads the app index and the current size of every live entry. Entries that
 * are malformed, duplicated or have no file are dropped and mark the index
 * dirty; the next mutation persists the pruned index. */
static storage_status_t storage_load_index(h2_lua_job_t *job) {
  const h2_pal_fs_api_t *fs = storage_fs(job);
  storage_scratch_t *scratch = job->host->storage_scratch;
  char path[H2_LUA_PATH_MAX];
  h2_pal_fs_stat_t stat;
  size_t text_length = 0u;
  size_t start = 0u;
  size_t i;
  storage_status_t status;
  int result;
  scratch->count = 0u;
  scratch->dirty = 0;
  storage_path(job, STORAGE_INDEX_NAME, path);
  result = h2_pal_fs_stat(fs, path, &stat);
  if (result == H2_PAL_ERR_NOT_FOUND) {
    return STORAGE_OK;
  }
  if (result != H2_PAL_OK || stat.is_dir) {
    return STORAGE_IO;
  }
  status = storage_read_exact(fs, path, scratch->text, scratch->text_capacity,
                              &text_length, 1);
  if (status != STORAGE_OK) {
    return status == STORAGE_NOT_FOUND ? STORAGE_OK : status;
  }
  if (stat.size > text_length) {
    scratch->dirty = 1;
  }
  for (i = 0u; i <= text_length; ++i) {
    size_t length;
    storage_entry_t *entry;
    if (i < text_length && scratch->text[i] != '\n') {
      continue;
    }
    length = i - start;
    if (length == 0u) {
      start = i + 1u;
      continue;
    }
    if (i == text_length || length > H2_LUA_STORAGE_NAME_MAX ||
        scratch->count >= scratch->entry_capacity) {
      /* An unterminated tail was cut off by the text capacity. */
      scratch->dirty = 1;
      start = i + 1u;
      continue;
    }
    entry = &scratch->entries[scratch->count];
    memcpy(entry->name, scratch->text + start, length);
    entry->name[length] = '\0';
    start = i + 1u;
    if (!h2_lua_storage_name_is_valid(entry->name, H2_LUA_STORAGE_NAME_MAX) ||
        storage_find(scratch, entry->name) >= 0) {
      scratch->dirty = 1;
      continue;
    }
    entry->size = 0u;
    scratch->count++;
  }
  for (i = 0u; i < scratch->count;) {
    storage_path(job, scratch->entries[i].name, path);
    result = h2_pal_fs_stat(fs, path, &stat);
    if (result == H2_PAL_ERR_NOT_FOUND) {
      storage_drop(scratch, i);
      scratch->dirty = 1;
      continue;
    }
    if (result != H2_PAL_OK || stat.is_dir) {
      return STORAGE_IO;
    }
    scratch->entries[i].size = stat.size;
    ++i;
  }
  return STORAGE_OK;
}

static storage_status_t storage_write_locked(h2_lua_job_t *job,
                                             const char *name, const char *data,
                                             size_t length) {
  storage_scratch_t *scratch = job->host->storage_scratch;
  uint64_t quota = job->host->config.storage.app_quota_bytes;
  uint64_t used;
  long index;
  storage_status_t status = storage_load_index(job);
  if (status != STORAGE_OK) {
    return status;
  }
  index = storage_find(scratch, name);
  if (index < 0 && scratch->count >= job->host->config.storage.app_max_files) {
    return STORAGE_TOO_MANY_FILES;
  }
  used =
      storage_used(scratch) - (index < 0 ? 0u : scratch->entries[index].size);
  if ((uint64_t)length > quota || used > quota - (uint64_t)length) {
    return STORAGE_QUOTA_EXCEEDED;
  }
  status = storage_ensure_dir(job);
  if (status != STORAGE_OK) {
    return status;
  }
  if (index < 0) {
    storage_entry_t *entry = &scratch->entries[scratch->count++];
    memcpy(entry->name, name, strlen(name) + 1u);
    entry->size = 0u;
    scratch->dirty = 1;
  }
  if (scratch->dirty) {
    status = storage_save_index(job);
    if (status != STORAGE_OK) {
      return status;
    }
  }
  status = storage_replace(job, STORAGE_DATA_TEMP_NAME, name, data, length);
  if (status != STORAGE_OK && index < 0) {
    /* The new name has no file; forget it again. Pruning on the next load
     * covers a failure of this rollback too. */
    storage_drop(scratch, scratch->count - 1u);
    (void)storage_save_index(job);
  }
  return status;
}

static storage_status_t storage_remove_locked(h2_lua_job_t *job,
                                              const char *name) {
  storage_scratch_t *scratch = job->host->storage_scratch;
  char path[H2_LUA_PATH_MAX];
  storage_status_t status = storage_load_index(job);
  long index;
  int result;
  if (status != STORAGE_OK) {
    return status;
  }
  index = storage_find(scratch, name);
  if (index < 0) {
    return STORAGE_NOT_FOUND;
  }
  storage_path(job, name, path);
  result = h2_pal_fs_remove(storage_fs(job), path);
  if (result != H2_PAL_OK && result != H2_PAL_ERR_NOT_FOUND) {
    return STORAGE_IO;
  }
  storage_drop(scratch, (size_t)index);
  /* The file is gone; a stale entry would be pruned by the next load. */
  (void)storage_save_index(job);
  return STORAGE_OK;
}

static storage_status_t storage_rename_locked(h2_lua_job_t *job,
                                              const char *old_name,
                                              const char *new_name) {
  storage_scratch_t *scratch = job->host->storage_scratch;
  char old_path[H2_LUA_PATH_MAX];
  char new_path[H2_LUA_PATH_MAX];
  storage_status_t status = storage_load_index(job);
  uint64_t size;
  long old_index;
  long new_index;
  if (status != STORAGE_OK) {
    return status;
  }
  old_index = storage_find(scratch, old_name);
  if (old_index < 0) {
    return STORAGE_NOT_FOUND;
  }
  if (strcmp(old_name, new_name) == 0) {
    return STORAGE_OK;
  }
  size = scratch->entries[old_index].size;
  new_index = storage_find(scratch, new_name);
  if (new_index < 0) {
    storage_entry_t *entry;
    if (scratch->count >= scratch->entry_capacity) {
      return STORAGE_TOO_MANY_FILES;
    }
    entry = &scratch->entries[scratch->count];
    new_index = (long)scratch->count++;
    memcpy(entry->name, new_name, strlen(new_name) + 1u);
    entry->size = 0u;
    status = storage_save_index(job);
    if (status != STORAGE_OK) {
      return status;
    }
  } else if (scratch->dirty) {
    status = storage_save_index(job);
    if (status != STORAGE_OK) {
      return status;
    }
  }
  storage_path(job, old_name, old_path);
  storage_path(job, new_name, new_path);
  status = from_pal(h2_pal_fs_rename(storage_fs(job), old_path, new_path));
  if (status != STORAGE_OK) {
    return status == STORAGE_NOT_FOUND ? STORAGE_IO : status;
  }
  scratch->entries[new_index].size = size;
  storage_drop(scratch, (size_t)old_index);
  (void)storage_save_index(job);
  return STORAGE_OK;
}

/* Returns the file size under the lock, then the caller allocates a Lua
 * buffer unlocked. A concurrent writer may replace the file in between, so
 * the read re-checks the size under the lock and asks for a retry. */
static storage_status_t storage_size_locked(h2_lua_job_t *job, const char *name,
                                            uint64_t *out_size) {
  storage_scratch_t *scratch = job->host->storage_scratch;
  storage_status_t status = storage_load_index(job);
  long index;
  if (status != STORAGE_OK) {
    return status;
  }
  index = storage_find(scratch, name);
  if (index < 0) {
    return STORAGE_NOT_FOUND;
  }
  *out_size = scratch->entries[index].size;
  return STORAGE_OK;
}

static storage_status_t storage_read_locked(h2_lua_job_t *job, const char *name,
                                            void *data, size_t expected_size) {
  const h2_pal_fs_api_t *fs = storage_fs(job);
  char path[H2_LUA_PATH_MAX];
  h2_pal_fs_stat_t stat;
  size_t read_size = 0u;
  storage_status_t status;
  storage_path(job, name, path);
  status = from_pal(h2_pal_fs_stat(fs, path, &stat));
  if (status != STORAGE_OK) {
    return status;
  }
  if (stat.is_dir) {
    return STORAGE_IO;
  }
  if (stat.size != expected_size) {
    return STORAGE_CHANGED;
  }
  return storage_read_exact(fs, path, data, expected_size, &read_size, 0);
}

static int push_failure(lua_State *state, storage_status_t status) {
  lua_pushnil(state);
  lua_pushstring(state, storage_message(status));
  return 2;
}

static int push_success(lua_State *state) {
  lua_pushboolean(state, 1);
  return 1;
}

/* Returns the name argument, or NULL when it is not a valid storage name. */
static const char *check_name(lua_State *state, int argument) {
  size_t length = 0u;
  const char *name = luaL_checklstring(state, argument, &length);
  if (length != strlen(name) ||
      !h2_lua_storage_name_is_valid(name, H2_LUA_STORAGE_NAME_MAX)) {
    return NULL;
  }
  return name;
}

static int storage_get_root_dir(lua_State *state) {
  if (!storage_available(storage_job(state))) {
    return push_failure(state, STORAGE_UNAVAILABLE);
  }
  /* App files are addressed relative to the app directory, so joining the
   * root with a name yields the bare name. */
  lua_pushliteral(state, "");
  return 1;
}

static int storage_join_path(lua_State *state) {
  int count = lua_gettop(state);
  luaL_Buffer buffer;
  int wrote = 0;
  int ends_with_slash = 0;
  int i;
  luaL_buffinit(state, &buffer);
  for (i = 1; i <= count; ++i) {
    size_t length = 0u;
    const char *part = luaL_checklstring(state, i, &length);
    size_t start = 0u;
    size_t end = length;
    if (wrote) {
      while (start < end && part[start] == '/') {
        ++start;
      }
    }
    while (end > start + 1u && part[end - 1u] == '/') {
      --end;
    }
    if (end <= start) {
      continue;
    }
    if (wrote && !ends_with_slash) {
      luaL_addchar(&buffer, '/');
    }
    luaL_addlstring(&buffer, part + start, end - start);
    wrote = 1;
    ends_with_slash = part[end - 1u] == '/';
  }
  luaL_pushresult(&buffer);
  return 1;
}

static int storage_exists(lua_State *state) {
  h2_lua_job_t *job = storage_job(state);
  const char *name = check_name(state, 1);
  uint64_t size = 0u;
  storage_status_t status;
  if (!storage_available(job) || name == NULL) {
    lua_pushboolean(state, 0);
    return 1;
  }
  status = storage_lock(job);
  if (status == STORAGE_OK) {
    status = storage_size_locked(job, name, &size);
    storage_unlock(job);
  }
  lua_pushboolean(state, status == STORAGE_OK);
  return 1;
}

static int storage_stat(lua_State *state) {
  h2_lua_job_t *job = storage_job(state);
  const char *name = check_name(state, 1);
  uint64_t size = 0u;
  storage_status_t status;
  if (!storage_available(job)) {
    return push_failure(state, STORAGE_UNAVAILABLE);
  }
  if (name == NULL) {
    return push_failure(state, STORAGE_INVALID_NAME);
  }
  status = storage_lock(job);
  if (status == STORAGE_OK) {
    status = storage_size_locked(job, name, &size);
    storage_unlock(job);
  }
  if (status != STORAGE_OK) {
    return push_failure(state, status);
  }
  lua_createtable(state, 0, 2);
  lua_pushliteral(state, "file");
  lua_setfield(state, -2, "type");
  lua_pushinteger(state, (lua_Integer)size);
  lua_setfield(state, -2, "size");
  return 1;
}

static int storage_read_file(lua_State *state) {
  h2_lua_job_t *job = storage_job(state);
  const char *name = check_name(state, 1);
  storage_status_t status = STORAGE_CHANGED;
  int attempt;
  if (!storage_available(job)) {
    return push_failure(state, STORAGE_UNAVAILABLE);
  }
  if (name == NULL) {
    return push_failure(state, STORAGE_INVALID_NAME);
  }
  for (attempt = 0;
       attempt < STORAGE_READ_ATTEMPTS && status == STORAGE_CHANGED;
       ++attempt) {
    uint64_t size = 0u;
    void *data;
    status = storage_lock(job);
    if (status == STORAGE_OK) {
      status = storage_size_locked(job, name, &size);
      storage_unlock(job);
    }
    if (status != STORAGE_OK) {
      break;
    }
    if (size > job->host->config.storage.app_quota_bytes) {
      status = STORAGE_IO;
      break;
    }
    /* Lua owns the buffer, so a memory error here leaks nothing. */
    data = lua_newuserdatauv(state, size == 0u ? 1u : (size_t)size, 0);
    status = storage_lock(job);
    if (status == STORAGE_OK) {
      status = storage_read_locked(job, name, data, (size_t)size);
      storage_unlock(job);
    }
    if (status == STORAGE_OK) {
      lua_pushlstring(state, data, (size_t)size);
      return 1;
    }
    lua_pop(state, 1);
  }
  return push_failure(state, status);
}

static int storage_write_file(lua_State *state) {
  h2_lua_job_t *job = storage_job(state);
  const char *name = check_name(state, 1);
  size_t length = 0u;
  const char *data = luaL_checklstring(state, 2, &length);
  storage_status_t status;
  if (!storage_available(job)) {
    return push_failure(state, STORAGE_UNAVAILABLE);
  }
  if (name == NULL) {
    return push_failure(state, STORAGE_INVALID_NAME);
  }
  status = storage_lock(job);
  if (status == STORAGE_OK) {
    status = storage_write_locked(job, name, data, length);
    storage_unlock(job);
  }
  return status == STORAGE_OK ? push_success(state)
                              : push_failure(state, status);
}

static int storage_remove(lua_State *state) {
  h2_lua_job_t *job = storage_job(state);
  const char *name = check_name(state, 1);
  storage_status_t status;
  if (!storage_available(job)) {
    return push_failure(state, STORAGE_UNAVAILABLE);
  }
  if (name == NULL) {
    return push_failure(state, STORAGE_INVALID_NAME);
  }
  status = storage_lock(job);
  if (status == STORAGE_OK) {
    status = storage_remove_locked(job, name);
    storage_unlock(job);
  }
  return status == STORAGE_OK ? push_success(state)
                              : push_failure(state, status);
}

static int storage_rename(lua_State *state) {
  h2_lua_job_t *job = storage_job(state);
  const char *old_name = check_name(state, 1);
  const char *new_name = check_name(state, 2);
  storage_status_t status;
  if (!storage_available(job)) {
    return push_failure(state, STORAGE_UNAVAILABLE);
  }
  if (old_name == NULL || new_name == NULL) {
    return push_failure(state, STORAGE_INVALID_NAME);
  }
  status = storage_lock(job);
  if (status == STORAGE_OK) {
    status = storage_rename_locked(job, old_name, new_name);
    storage_unlock(job);
  }
  return status == STORAGE_OK ? push_success(state)
                              : push_failure(state, status);
}

static int storage_listdir(lua_State *state) {
  h2_lua_job_t *job = storage_job(state);
  size_t path_length = 0u;
  const char *path = luaL_optlstring(state, 1, "", &path_length);
  storage_entry_t *entries;
  size_t count = 0u;
  size_t i;
  storage_status_t status;
  if (!storage_available(job)) {
    return push_failure(state, STORAGE_UNAVAILABLE);
  }
  if (path_length != 0u) {
    /* The app directory is flat; only its root can be listed. */
    return push_failure(
        state, h2_lua_storage_name_is_valid(path, H2_LUA_STORAGE_NAME_MAX)
                   ? STORAGE_NOT_FOUND
                   : STORAGE_INVALID_NAME);
  }
  entries = lua_newuserdatauv(
      state, job->host->storage_scratch->entry_capacity * sizeof(*entries), 0);
  status = storage_lock(job);
  if (status == STORAGE_OK) {
    status = storage_load_index(job);
    if (status == STORAGE_OK) {
      count = job->host->storage_scratch->count;
      memcpy(entries, job->host->storage_scratch->entries,
             count * sizeof(*entries));
    }
    storage_unlock(job);
  }
  if (status != STORAGE_OK) {
    return push_failure(state, status);
  }
  lua_createtable(state, (int)count, 0);
  for (i = 0u; i < count; ++i) {
    lua_createtable(state, 0, 3);
    lua_pushstring(state, entries[i].name);
    lua_setfield(state, -2, "name");
    lua_pushliteral(state, "file");
    lua_setfield(state, -2, "type");
    lua_pushinteger(state, (lua_Integer)entries[i].size);
    lua_setfield(state, -2, "size");
    lua_rawseti(state, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int storage_get_free_space(lua_State *state) {
  h2_lua_job_t *job = storage_job(state);
  uint64_t quota;
  uint64_t used = 0u;
  storage_status_t status;
  if (!storage_available(job)) {
    return push_failure(state, STORAGE_UNAVAILABLE);
  }
  quota = job->host->config.storage.app_quota_bytes;
  status = storage_lock(job);
  if (status == STORAGE_OK) {
    status = storage_load_index(job);
    used = storage_used(job->host->storage_scratch);
    storage_unlock(job);
  }
  if (status != STORAGE_OK) {
    return push_failure(state, status);
  }
  /* Files written outside this module can push usage past the quota. */
  used = used > quota ? quota : used;
  lua_createtable(state, 0, 3);
  lua_pushinteger(state, (lua_Integer)quota);
  lua_setfield(state, -2, "total");
  lua_pushinteger(state, (lua_Integer)(quota - used));
  lua_setfield(state, -2, "free");
  lua_pushinteger(state, (lua_Integer)used);
  lua_setfield(state, -2, "used");
  return 1;
}

static void set_storage_function(lua_State *state, const char *name,
                                 lua_CFunction function, h2_lua_job_t *job) {
  lua_pushlightuserdata(state, job);
  lua_pushcclosure(state, function, 1);
  lua_setfield(state, -2, name);
}

int h2_lua_open_storage(lua_State *state) {
  h2_lua_job_t *job = storage_job(state);
  lua_createtable(state, 0, 10);
  set_storage_function(state, "get_root_dir", storage_get_root_dir, job);
  set_storage_function(state, "join_path", storage_join_path, job);
  set_storage_function(state, "exists", storage_exists, job);
  set_storage_function(state, "stat", storage_stat, job);
  set_storage_function(state, "read_file", storage_read_file, job);
  set_storage_function(state, "write_file", storage_write_file, job);
  set_storage_function(state, "listdir", storage_listdir, job);
  set_storage_function(state, "remove", storage_remove, job);
  set_storage_function(state, "rename", storage_rename, job);
  set_storage_function(state, "get_free_space", storage_get_free_space, job);
  return 1;
}
