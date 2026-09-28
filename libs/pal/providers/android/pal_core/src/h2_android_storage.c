#include "h2_android_platform.h"
#include "h2_posix_pal_core.h"
#include "h2_sqlite.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct h2_android_storage {
  h2_posix_host_fs_t *fs;
  h2_sqlite_t *pref;
};
static int ensure_directory(const char *path) {
  if (mkdir(path, 0700) == 0)
    return H2_PAL_OK;
  if (errno != EEXIST)
    return H2_PAL_ERR_IO;
  struct stat info;
  return lstat(path, &info) == 0 && S_ISDIR(info.st_mode)
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}
h2_pal_result_t h2_android_storage_create(const char *directory,
                                          const char *portable_root,
                                          h2_android_storage_t **out) {
  if (out)
    *out = NULL;
  if (!out || !directory || directory[0] != '/' || !portable_root)
    return H2_PAL_ERR_INVALID_ARG;
  size_t length = strlen(directory);
  if (length > 4096u)
    return H2_PAL_ERR_INVALID_ARG;
  char *path = malloc(length + 24u);
  h2_android_storage_t *owner = calloc(1, sizeof(*owner));
  if (!path || !owner) {
    free(path);
    free(owner);
    return H2_PAL_ERR_NO_MEMORY;
  }
  int rc = ensure_directory(directory);
  if (!rc) {
    snprintf(path, length + 24u, "%s/files", directory);
    rc = ensure_directory(path);
  }
  if (!rc) {
    const char *sources[] = {path}, *targets[] = {portable_root};
    rc = h2_posix_host_fs_create(sources, targets, 1, &owner->fs);
  }
  if (!rc) {
    snprintf(path, length + 24u, "%s/preferences.sqlite", directory);
    const h2_sqlite_config_t config = {.path = path};
    rc = h2_sqlite_create(&config, &owner->pref);
  }
  free(path);
  if (rc) {
    h2_android_storage_destroy(owner);
    return (h2_pal_result_t)rc;
  }
  *out = owner;
  return H2_PAL_OK;
}
const h2_pal_fs_api_t *h2_android_storage_fs_api(h2_android_storage_t *owner) {
  return owner ? h2_posix_host_fs_api(owner->fs) : NULL;
}
const h2_pal_pref_api_t *
h2_android_storage_pref_api(h2_android_storage_t *owner) {
  return owner ? h2_sqlite_pref_api(owner->pref) : NULL;
}
void h2_android_storage_destroy(h2_android_storage_t *owner) {
  if (!owner)
    return;
  h2_sqlite_destroy(owner->pref);
  h2_posix_host_fs_destroy(owner->fs);
  free(owner);
}
