#ifndef H2_GIZCLAW_LUA_APP_CATALOG_INTERNAL_H
#define H2_GIZCLAW_LUA_APP_CATALOG_INTERNAL_H
#include "h2_gizclaw_lua_app_catalog.h"
#include "h2_sha256.h"
#include "yyjson.h"

#define H2_GIZCLAW_LUA_APP_MANIFEST_MAX 32768u
#define H2_GIZCLAW_LUA_APPS_INDEX_MAX (256u * 1024u)

typedef struct app_file {
    char path[H2_GIZCLAW_LUA_APP_PATH_MAX];
    uint64_t size;
    char sha256[65];
} app_file_t;

typedef struct app_manifest {
    h2_gizclaw_lua_app_info_t info;
    char entry[H2_GIZCLAW_LUA_APP_PATH_MAX];
    char data_dir[33];
    size_t count;
    uint64_t bytes;
    app_file_t files[H2_GIZCLAW_LUA_APP_FILES_MAX];
} app_manifest_t;

typedef struct app_record {
    h2_gizclaw_lua_app_info_t info;
    uint64_t bytes;
    int slot; /* -2 is uninstalled; -1 is preinstalled; 0/1 are writable. */
    bool preinstalled;
} app_record_t;

struct h2_gizclaw_lua_app_catalog {
    h2_gizclaw_lua_app_catalog_config_t config;
    char root[H2_GIZCLAW_LUA_APP_PATH_MAX];
    h2_pal_mutex_t *mutex;
    bool ready;
    int close_error;
    /* PAL close does not identify whether an error consumed its handle. Keep
     * the manager/dependencies alive; never retry an ambiguously closed file.
     */
    h2_pal_fs_file_t *unclosed_file;
    yyjson_alc json_allocator;
    size_t count;
    app_record_t records[H2_GIZCLAW_LUA_APPS_MAX];
};

bool h2_gizclaw_lua_app_catalog_path_valid(const char *path);
int h2_gizclaw_lua_app_catalog_join(char out[H2_GIZCLAW_LUA_APP_PATH_MAX],
                                    const char *root, const char *name);
int h2_gizclaw_lua_app_catalog_read(h2_gizclaw_lua_app_catalog_t *apps,
                                    const char *path, size_t limit,
                                    uint8_t **out, size_t *out_size);
int h2_gizclaw_lua_app_catalog_write(h2_gizclaw_lua_app_catalog_t *apps,
                                     const char *path, const void *data,
                                     size_t size);
int h2_gizclaw_lua_app_catalog_mkdirs(h2_gizclaw_lua_app_catalog_t *apps,
                                      const char *file_path);
int h2_gizclaw_lua_app_catalog_manifest(h2_gizclaw_lua_app_catalog_t *apps,
                                        yyjson_val *value, app_manifest_t *out);
int h2_gizclaw_lua_app_catalog_resolve(h2_gizclaw_lua_app_catalog_t *apps,
                                       app_manifest_t *manifest,
                                       const char *root);
int h2_gizclaw_lua_app_catalog_publish(h2_gizclaw_lua_app_catalog_t *apps,
                                       const app_record_t *record,
                                       size_t position);
int h2_gizclaw_lua_app_catalog_check_slot(h2_gizclaw_lua_app_catalog_t *apps,
                                          const char *root, const char *app_id);
int h2_gizclaw_lua_app_catalog_remove_slot(h2_gizclaw_lua_app_catalog_t *apps,
                                           const char *root,
                                           const char *app_id);
int h2_gizclaw_lua_app_catalog_try_lock(h2_gizclaw_lua_app_catalog_t *apps);
void h2_gizclaw_lua_app_catalog_unlock(h2_gizclaw_lua_app_catalog_t *apps);
int h2_gizclaw_lua_app_catalog_find(const h2_gizclaw_lua_app_catalog_t *apps,
                                    const char *id);
int h2_gizclaw_lua_app_catalog_close(h2_gizclaw_lua_app_catalog_t *apps,
                                     h2_pal_fs_file_t *file);
#endif
