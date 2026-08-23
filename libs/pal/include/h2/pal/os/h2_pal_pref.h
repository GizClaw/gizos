#ifndef H2_PAL_PREF_H
#define H2_PAL_PREF_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_pal_pref_namespace h2_pal_pref_namespace_t;
typedef struct h2_pal_pref_cursor h2_pal_pref_cursor_t;

typedef enum h2_pal_pref_open_mode {
    H2_PAL_PREF_OPEN_READ_ONLY = 1,
    H2_PAL_PREF_OPEN_READ_WRITE = 2,
} h2_pal_pref_open_mode_t;

typedef enum h2_pal_pref_entry_type {
    H2_PAL_PREF_ENTRY_UNKNOWN = 0,
    H2_PAL_PREF_ENTRY_BLOB = 1,
    H2_PAL_PREF_ENTRY_STRING = 2,
    H2_PAL_PREF_ENTRY_U32 = 3,
    H2_PAL_PREF_ENTRY_I32 = 4,
    H2_PAL_PREF_ENTRY_BOOL = 5,
} h2_pal_pref_entry_type_t;

typedef struct h2_pal_pref_entry {
    const char *key;
    h2_pal_pref_entry_type_t type;
    size_t value_size;
} h2_pal_pref_entry_t;

typedef int (*h2_pal_pref_api_open_fn)(
    void *user,
    const char *name_space,
    h2_pal_pref_open_mode_t mode,
    h2_pal_pref_namespace_t **out_namespace);

typedef struct h2_pal_pref_vtable {
    h2_pal_pref_api_open_fn open;
} h2_pal_pref_vtable_t;

typedef struct h2_pal_pref_api {
    void *user;
    const h2_pal_pref_vtable_t *vtable;
} h2_pal_pref_api_t;

static inline int h2_pal_pref_open(
    const h2_pal_pref_api_t *api,
    const char *name_space,
    h2_pal_pref_open_mode_t mode,
    h2_pal_pref_namespace_t **out_namespace) {
    if (api == NULL || api->vtable == NULL || api->vtable->open == NULL ||
        name_space == NULL || out_namespace == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->open(api->user, name_space, mode, out_namespace);
}

typedef int (*h2_pal_pref_namespace_close_fn)(h2_pal_pref_namespace_t *ns);
typedef int (*h2_pal_pref_namespace_get_blob_fn)(
    h2_pal_pref_namespace_t *ns,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    void **out_data,
    size_t *out_len);
typedef int (*h2_pal_pref_namespace_set_blob_fn)(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    const void *data,
    size_t data_len);
typedef int (*h2_pal_pref_namespace_get_string_fn)(
    h2_pal_pref_namespace_t *ns,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    char **out_value);
typedef int (*h2_pal_pref_namespace_set_string_fn)(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    const char *value);
typedef int (*h2_pal_pref_namespace_get_u32_fn)(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    uint32_t *out_value);
typedef int (*h2_pal_pref_namespace_set_u32_fn)(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    uint32_t value);
typedef int (*h2_pal_pref_namespace_get_i32_fn)(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    int32_t *out_value);
typedef int (*h2_pal_pref_namespace_set_i32_fn)(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    int32_t value);
typedef int (*h2_pal_pref_namespace_get_bool_fn)(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    int *out_value);
typedef int (*h2_pal_pref_namespace_set_bool_fn)(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    int value);
typedef int (*h2_pal_pref_namespace_remove_fn)(
    h2_pal_pref_namespace_t *ns,
    const char *key);
typedef int (*h2_pal_pref_namespace_clear_fn)(h2_pal_pref_namespace_t *ns);
typedef int (*h2_pal_pref_namespace_commit_fn)(h2_pal_pref_namespace_t *ns);
typedef int (*h2_pal_pref_namespace_iterate_fn)(
    h2_pal_pref_namespace_t *ns,
    h2_pal_pref_cursor_t **cursor,
    h2_pal_pref_entry_t *out_entry);
typedef int (*h2_pal_pref_namespace_iterate_close_fn)(
    h2_pal_pref_namespace_t *ns,
    h2_pal_pref_cursor_t **cursor);

struct h2_pal_pref_namespace {
    void *user;

    h2_pal_pref_namespace_close_fn close;
    h2_pal_pref_namespace_get_blob_fn get_blob;
    h2_pal_pref_namespace_set_blob_fn set_blob;
    h2_pal_pref_namespace_get_string_fn get_string;
    h2_pal_pref_namespace_set_string_fn set_string;
    h2_pal_pref_namespace_get_u32_fn get_u32;
    h2_pal_pref_namespace_set_u32_fn set_u32;
    h2_pal_pref_namespace_get_i32_fn get_i32;
    h2_pal_pref_namespace_set_i32_fn set_i32;
    h2_pal_pref_namespace_get_bool_fn get_bool;
    h2_pal_pref_namespace_set_bool_fn set_bool;
    h2_pal_pref_namespace_remove_fn remove;
    h2_pal_pref_namespace_clear_fn clear;
    h2_pal_pref_namespace_commit_fn commit;
    h2_pal_pref_namespace_iterate_fn iterate;
    h2_pal_pref_namespace_iterate_close_fn iterate_close;
};

#ifdef __cplusplus
}
#endif

#endif
