#define _POSIX_C_SOURCE 200809L
#include "h2_fruit_slice_desktop_pref.h"
#include "h2_fruit_slice.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { PREF_PATH_MAX = 4096 };

static int mkdir_tree(char* path) {
  for (char* p = path + 1;; ++p) {
    if (*p != '/' && *p != '\0') continue;
    const char separator = *p;
    *p = '\0';
    const int created = mkdir(path, 0700);
    int rc = H2_PAL_OK;
    if (created != 0) {
      struct stat info;
      if (errno != EEXIST)
        rc = H2_PAL_ERR_IO;
      else if (stat(path, &info) != 0 || !S_ISDIR(info.st_mode))
        rc = H2_PAL_ERR_INVALID_STATE;
    }
    *p = separator;
    if (rc != H2_PAL_OK || separator == '\0') return rc;
  }
}

int h2_fruit_slice_desktop_pref_create(const char* root, h2_sqlite_t** out) {
  if (!out) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  char directory[PREF_PATH_MAX];
  int length;
  if (!root || !*root) root = getenv("H2_DESKTOP_STORAGE_ROOT");
  if (root && *root) {
    length = snprintf(directory, sizeof(directory), "%s", root);
  } else {
    const char* home = getenv("HOME");
#if defined(__APPLE__)
    if (!home || home[0] != '/') return H2_PAL_ERR_INVALID_STATE;
    length = snprintf(directory, sizeof(directory),
                      "%s/Library/Application Support/FruitSlice", home);
#else
    const char* data = getenv("XDG_DATA_HOME");
    if (data && data[0] == '/') {
      length = snprintf(directory, sizeof(directory), "%s/gizclaw/fruit-slice",
                        data);
    } else {
      if (!home || home[0] != '/') return H2_PAL_ERR_INVALID_STATE;
      length = snprintf(directory, sizeof(directory),
                        "%s/.local/share/gizclaw/fruit-slice", home);
    }
#endif
  }
  if (length <= 0 || (size_t)length >= sizeof(directory) || directory[0] != '/')
    return H2_PAL_ERR_INVALID_ARG;
  int rc = mkdir_tree(directory);
  if (rc != H2_PAL_OK) return rc;
  char path[PREF_PATH_MAX];
  length = snprintf(path, sizeof(path), "%s/preferences.sqlite", directory);
  if (length <= 0 || (size_t)length >= sizeof(path))
    return H2_PAL_ERR_INVALID_ARG;
  const h2_sqlite_config_t config = {.path = path};
  h2_sqlite_t* provider = NULL;
  rc = h2_sqlite_create(&config, &provider);
  if (rc != H2_PAL_OK) return rc;
  h2_pal_pref_namespace_t* namespace = NULL;
  rc = h2_pal_pref_open(h2_sqlite_pref_api(provider),
                        H2_FRUIT_SLICE_PREF_NAMESPACE,
                        H2_PAL_PREF_OPEN_READ_WRITE, &namespace);
  if (rc == H2_PAL_OK) rc = namespace->close(namespace);
  if (rc != H2_PAL_OK) {
    h2_sqlite_destroy(provider);
    return rc;
  }
  *out = provider;
  return H2_PAL_OK;
}
