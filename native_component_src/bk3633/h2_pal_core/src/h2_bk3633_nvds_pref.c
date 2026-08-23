#include "h2_bk3633_platform_core.h"
#include "h2_bk3633_pal_storage_internal.h"

#include "h2/pal/h2_pal_unsupported.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(BK3633)
#include "nvds.h"
#endif

#define H2_BK3633_NVDS_PREF_HANDLE_COUNT 4u

typedef struct h2_bk3633_nvds_pref_state h2_bk3633_nvds_pref_state_t;
typedef struct h2_bk3633_nvds_pref_namespace h2_bk3633_nvds_pref_namespace_t;

struct h2_pal_pref_cursor {
    h2_bk3633_nvds_pref_namespace_t *owner;
    size_t next_index;
};

struct h2_bk3633_nvds_pref_namespace {
    h2_pal_pref_namespace_t api;
    h2_bk3633_nvds_pref_state_t *provider;
    const char *name_space;
    h2_pal_pref_open_mode_t mode;
    bool active;
    struct h2_pal_pref_cursor cursor;
};

struct h2_bk3633_nvds_pref_state {
    h2_pal_pref_api_t api;
    const h2_bk3633_nvds_pref_entry_t *entries;
    size_t entry_count;
    h2_bk3633_nvds_driver_t driver;
    h2_bk3633_nvds_pref_namespace_t handles[H2_BK3633_NVDS_PREF_HANDLE_COUNT];
    bool ready;
};

static h2_bk3633_nvds_pref_state_t s_nvds_pref;

static size_t pref_bounded_strlen(const char *text, size_t limit)
{
    size_t len = 0u;
    while (len < limit && text[len] != '\0') {
        ++len;
    }
    return len;
}

static h2_pal_result_t nvds_result(h2_bk3633_nvds_status_t status)
{
    switch (status) {
    case H2_BK3633_NVDS_STATUS_OK:
        return H2_PAL_OK;
    case H2_BK3633_NVDS_STATUS_NOT_FOUND:
        return H2_PAL_ERR_NOT_FOUND;
    case H2_BK3633_NVDS_STATUS_NO_SPACE:
        return H2_PAL_ERR_NO_SPACE;
    case H2_BK3633_NVDS_STATUS_LOCKED:
        return H2_PAL_ERR_INVALID_STATE;
    case H2_BK3633_NVDS_STATUS_LENGTH:
    case H2_BK3633_NVDS_STATUS_CORRUPT:
    case H2_BK3633_NVDS_STATUS_FAIL:
    default:
        return H2_PAL_ERR_IO;
    }
}

static bool pref_type_valid(h2_pal_pref_entry_type_t type)
{
    return type >= H2_PAL_PREF_ENTRY_BLOB &&
           type <= H2_PAL_PREF_ENTRY_BOOL;
}

static bool pref_entry_size_valid(const h2_bk3633_nvds_pref_entry_t *entry)
{
    if (entry->max_value_size == 0u ||
        entry->max_value_size > H2_BK3633_NVDS_VALUE_SIZE_MAX) {
        return false;
    }
    if (entry->type == H2_PAL_PREF_ENTRY_U32 ||
        entry->type == H2_PAL_PREF_ENTRY_I32) {
        return entry->max_value_size == 4u;
    }
    if (entry->type == H2_PAL_PREF_ENTRY_BOOL) {
        return entry->max_value_size == 1u;
    }
    return true;
}

static h2_pal_result_t pref_mapping_validate(
    const h2_bk3633_nvds_pref_entry_t *entries,
    size_t entry_count)
{
    size_t index;
    size_t other;

    if (entry_count != 0u && entries == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (index = 0u; index < entry_count; ++index) {
        const h2_bk3633_nvds_pref_entry_t *entry = &entries[index];
        if (entry->name_space == NULL || entry->name_space[0] == '\0' ||
            entry->key == NULL || entry->key[0] == '\0' ||
            !pref_type_valid(entry->type) ||
            !pref_entry_size_valid(entry) ||
            entry->nvds_tag < H2_BK3633_NVDS_APPLICATION_TAG_MIN ||
            entry->nvds_tag > H2_BK3633_NVDS_APPLICATION_TAG_MAX) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        for (other = 0u; other < index; ++other) {
            const h2_bk3633_nvds_pref_entry_t *prior = &entries[other];
            if (prior->nvds_tag == entry->nvds_tag ||
                (strcmp(prior->name_space, entry->name_space) == 0 &&
                 strcmp(prior->key, entry->key) == 0)) {
                return H2_PAL_ERR_INVALID_ARG;
            }
        }
    }
    return H2_PAL_OK;
}

static const h2_bk3633_nvds_pref_entry_t *pref_find_entry(
    const h2_bk3633_nvds_pref_namespace_t *name_space,
    const char *key)
{
    size_t index;

    if (name_space == NULL || !name_space->active || key == NULL ||
        key[0] == '\0') {
        return NULL;
    }
    for (index = 0u; index < name_space->provider->entry_count; ++index) {
        const h2_bk3633_nvds_pref_entry_t *entry =
            &name_space->provider->entries[index];
        if (strcmp(entry->name_space, name_space->name_space) == 0 &&
            strcmp(entry->key, key) == 0) {
            return entry;
        }
    }
    return NULL;
}

static h2_pal_result_t pref_require_entry(
    h2_pal_pref_namespace_t *api,
    const char *key,
    h2_pal_pref_entry_type_t type,
    h2_bk3633_nvds_pref_namespace_t **out_name_space,
    const h2_bk3633_nvds_pref_entry_t **out_entry)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    const h2_bk3633_nvds_pref_entry_t *entry;

    if (api == NULL || api->user == NULL || key == NULL ||
        out_name_space == NULL || out_entry == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    name_space = (h2_bk3633_nvds_pref_namespace_t *)api->user;
    if (!name_space->active || name_space->provider == NULL ||
        !name_space->provider->ready) {
        return H2_PAL_ERR_CLOSED;
    }
    entry = pref_find_entry(name_space, key);
    if (entry == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (entry->type != type) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_name_space = name_space;
    *out_entry = entry;
    return H2_PAL_OK;
}

static h2_pal_result_t pref_require_write(
    h2_bk3633_nvds_pref_namespace_t *name_space)
{
    return name_space->mode == H2_PAL_PREF_OPEN_READ_WRITE ?
        H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t pref_read_raw(
    h2_bk3633_nvds_pref_namespace_t *name_space,
    const h2_bk3633_nvds_pref_entry_t *entry,
    uint8_t *data,
    size_t capacity,
    size_t *out_len)
{
    uint8_t len;
    h2_pal_result_t rc;

    if (capacity > H2_BK3633_NVDS_VALUE_SIZE_MAX || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    len = (uint8_t)capacity;
    rc = nvds_result(name_space->provider->driver.get(
        name_space->provider->driver.user, entry->nvds_tag, &len, data));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((size_t)len > entry->max_value_size || (size_t)len > capacity) {
        return H2_PAL_ERR_IO;
    }
    *out_len = len;
    return H2_PAL_OK;
}

static h2_pal_result_t pref_write_raw(
    h2_bk3633_nvds_pref_namespace_t *name_space,
    const h2_bk3633_nvds_pref_entry_t *entry,
    const uint8_t *data,
    size_t len)
{
    h2_pal_result_t rc = pref_require_write(name_space);
    static const uint8_t empty = 0u;

    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (len > entry->max_value_size ||
        len > H2_BK3633_NVDS_VALUE_SIZE_MAX ||
        (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return nvds_result(name_space->provider->driver.put(
        name_space->provider->driver.user,
        entry->nvds_tag,
        (uint8_t)len,
        len == 0u ? &empty : data));
}

static bool pref_utf8_valid(const uint8_t *data, size_t len)
{
    size_t index = 0u;
    while (index < len) {
        uint8_t first = data[index++];
        uint32_t codepoint;
        size_t continuation;
        if (first <= 0x7fu) {
            if (first == 0u) {
                return false;
            }
            continue;
        }
        if (first >= 0xc2u && first <= 0xdfu) {
            codepoint = (uint32_t)(first & 0x1fu);
            continuation = 1u;
        } else if (first >= 0xe0u && first <= 0xefu) {
            codepoint = (uint32_t)(first & 0x0fu);
            continuation = 2u;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            codepoint = (uint32_t)(first & 0x07u);
            continuation = 3u;
        } else {
            return false;
        }
        if (continuation > len - index) {
            return false;
        }
        while (continuation-- != 0u) {
            uint8_t next = data[index++];
            if ((next & 0xc0u) != 0x80u) {
                return false;
            }
            codepoint = (codepoint << 6) | (uint32_t)(next & 0x3fu);
        }
        if ((codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
            codepoint > 0x10ffffu ||
            (codepoint < 0x800u && first >= 0xe0u) ||
            (codepoint < 0x10000u && first >= 0xf0u)) {
            return false;
        }
    }
    return true;
}

static int pref_close(h2_pal_pref_namespace_t *api)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    if (api == NULL || api->user == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    name_space = (h2_bk3633_nvds_pref_namespace_t *)api->user;
    if (!name_space->active) {
        return H2_PAL_OK;
    }
    name_space->active = false;
    name_space->cursor.owner = NULL;
    name_space->cursor.next_index = 0u;
    return H2_PAL_OK;
}

static int pref_get_blob(
    h2_pal_pref_namespace_t *api,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    void **out_data,
    size_t *out_len)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    const h2_bk3633_nvds_pref_entry_t *entry;
    uint8_t scratch[H2_BK3633_NVDS_VALUE_SIZE_MAX];
    size_t len = 0u;
    void *result;
    h2_pal_result_t rc;

    if (allocator == NULL || out_data == NULL || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_len = 0u;
    rc = pref_require_entry(
        api, key, H2_PAL_PREF_ENTRY_BLOB, &name_space, &entry);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = pref_read_raw(name_space, entry, scratch, entry->max_value_size, &len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    result = h2_pal_mem_alloc(allocator, len == 0u ? 1u : len);
    if (result == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (len != 0u) {
        memcpy(result, scratch, len);
    }
    *out_data = result;
    *out_len = len;
    return H2_PAL_OK;
}

static int pref_set_blob(
    h2_pal_pref_namespace_t *api,
    const char *key,
    const void *data,
    size_t data_len)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    const h2_bk3633_nvds_pref_entry_t *entry;
    h2_pal_result_t rc = pref_require_entry(
        api, key, H2_PAL_PREF_ENTRY_BLOB, &name_space, &entry);
    return rc == H2_PAL_OK ?
        pref_write_raw(name_space, entry, (const uint8_t *)data, data_len) : rc;
}

static int pref_get_string(
    h2_pal_pref_namespace_t *api,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    char **out_value)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    const h2_bk3633_nvds_pref_entry_t *entry;
    uint8_t scratch[H2_BK3633_NVDS_VALUE_SIZE_MAX];
    size_t len = 0u;
    char *result;
    h2_pal_result_t rc;

    if (allocator == NULL || out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = NULL;
    rc = pref_require_entry(
        api, key, H2_PAL_PREF_ENTRY_STRING, &name_space, &entry);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = pref_read_raw(name_space, entry, scratch, entry->max_value_size, &len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (!pref_utf8_valid(scratch, len)) {
        return H2_PAL_ERR_IO;
    }
    result = (char *)h2_pal_mem_alloc(allocator, len + 1u);
    if (result == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(result, scratch, len);
    result[len] = '\0';
    *out_value = result;
    return H2_PAL_OK;
}

static int pref_set_string(
    h2_pal_pref_namespace_t *api,
    const char *key,
    const char *value)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    const h2_bk3633_nvds_pref_entry_t *entry;
    size_t len;
    h2_pal_result_t rc;

    if (value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = pref_require_entry(
        api, key, H2_PAL_PREF_ENTRY_STRING, &name_space, &entry);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    len = pref_bounded_strlen(value, entry->max_value_size + 1u);
    if (len > entry->max_value_size ||
        !pref_utf8_valid((const uint8_t *)value, len)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return pref_write_raw(name_space, entry, (const uint8_t *)value, len);
}

static int pref_get_u32_typed(
    h2_pal_pref_namespace_t *api,
    const char *key,
    h2_pal_pref_entry_type_t type,
    uint32_t *out_value)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    const h2_bk3633_nvds_pref_entry_t *entry;
    uint8_t bytes[4];
    size_t len = 0u;
    h2_pal_result_t rc;

    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = pref_require_entry(api, key, type, &name_space, &entry);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = pref_read_raw(name_space, entry, bytes, sizeof(bytes), &len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (len != sizeof(bytes)) {
        return H2_PAL_ERR_IO;
    }
    *out_value = (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
    return H2_PAL_OK;
}

static int pref_set_u32_typed(
    h2_pal_pref_namespace_t *api,
    const char *key,
    h2_pal_pref_entry_type_t type,
    uint32_t value)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    const h2_bk3633_nvds_pref_entry_t *entry;
    uint8_t bytes[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 24),
    };
    h2_pal_result_t rc = pref_require_entry(
        api, key, type, &name_space, &entry);
    return rc == H2_PAL_OK ?
        pref_write_raw(name_space, entry, bytes, sizeof(bytes)) : rc;
}

static int pref_get_u32(
    h2_pal_pref_namespace_t *api,
    const char *key,
    uint32_t *out_value)
{
    return pref_get_u32_typed(api, key, H2_PAL_PREF_ENTRY_U32, out_value);
}

static int pref_set_u32(
    h2_pal_pref_namespace_t *api,
    const char *key,
    uint32_t value)
{
    return pref_set_u32_typed(api, key, H2_PAL_PREF_ENTRY_U32, value);
}

static int pref_get_i32(
    h2_pal_pref_namespace_t *api,
    const char *key,
    int32_t *out_value)
{
    uint32_t bits = 0u;
    h2_pal_result_t rc;
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = pref_get_u32_typed(api, key, H2_PAL_PREF_ENTRY_I32, &bits);
    if (rc == H2_PAL_OK) {
        memcpy(out_value, &bits, sizeof(bits));
    }
    return rc;
}

static int pref_set_i32(
    h2_pal_pref_namespace_t *api,
    const char *key,
    int32_t value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return pref_set_u32_typed(api, key, H2_PAL_PREF_ENTRY_I32, bits);
}

static int pref_get_bool(
    h2_pal_pref_namespace_t *api,
    const char *key,
    int *out_value)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    const h2_bk3633_nvds_pref_entry_t *entry;
    uint8_t value = 0u;
    size_t len = 0u;
    h2_pal_result_t rc;
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = pref_require_entry(
        api, key, H2_PAL_PREF_ENTRY_BOOL, &name_space, &entry);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = pref_read_raw(name_space, entry, &value, sizeof(value), &len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (len != 1u || value > 1u) {
        return H2_PAL_ERR_IO;
    }
    *out_value = value != 0u;
    return H2_PAL_OK;
}

static int pref_set_bool(
    h2_pal_pref_namespace_t *api,
    const char *key,
    int value)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    const h2_bk3633_nvds_pref_entry_t *entry;
    uint8_t stored = value != 0 ? 1u : 0u;
    h2_pal_result_t rc = pref_require_entry(
        api, key, H2_PAL_PREF_ENTRY_BOOL, &name_space, &entry);
    return rc == H2_PAL_OK ?
        pref_write_raw(name_space, entry, &stored, sizeof(stored)) : rc;
}

static int pref_remove(h2_pal_pref_namespace_t *api, const char *key)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    const h2_bk3633_nvds_pref_entry_t *entry;
    h2_pal_result_t rc;
    if (api == NULL || api->user == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    name_space = (h2_bk3633_nvds_pref_namespace_t *)api->user;
    if (!name_space->active) {
        return H2_PAL_ERR_CLOSED;
    }
    rc = pref_require_write(name_space);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    entry = pref_find_entry(name_space, key);
    if (entry == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    return nvds_result(name_space->provider->driver.del(
        name_space->provider->driver.user, entry->nvds_tag));
}

static int pref_clear(h2_pal_pref_namespace_t *api)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    size_t index;
    h2_pal_result_t rc;
    if (api == NULL || api->user == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    name_space = (h2_bk3633_nvds_pref_namespace_t *)api->user;
    if (!name_space->active) {
        return H2_PAL_ERR_CLOSED;
    }
    rc = pref_require_write(name_space);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (index = 0u; index < name_space->provider->entry_count; ++index) {
        const h2_bk3633_nvds_pref_entry_t *entry =
            &name_space->provider->entries[index];
        if (strcmp(entry->name_space, name_space->name_space) != 0) {
            continue;
        }
        rc = nvds_result(name_space->provider->driver.del(
            name_space->provider->driver.user, entry->nvds_tag));
        if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static int pref_commit(h2_pal_pref_namespace_t *api)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    if (api == NULL || api->user == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    name_space = (h2_bk3633_nvds_pref_namespace_t *)api->user;
    return name_space->active ? H2_PAL_OK : H2_PAL_ERR_CLOSED;
}

static int pref_iterate(
    h2_pal_pref_namespace_t *api,
    h2_pal_pref_cursor_t **cursor,
    h2_pal_pref_entry_t *out_entry)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    h2_pal_pref_cursor_t *current;
    if (api == NULL || api->user == NULL || cursor == NULL ||
        out_entry == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    name_space = (h2_bk3633_nvds_pref_namespace_t *)api->user;
    if (!name_space->active) {
        return H2_PAL_ERR_CLOSED;
    }
    if (*cursor == NULL) {
        name_space->cursor.owner = name_space;
        name_space->cursor.next_index = 0u;
        *cursor = &name_space->cursor;
    }
    current = *cursor;
    if (current != &name_space->cursor || current->owner != name_space) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    while (current->next_index < name_space->provider->entry_count) {
        const h2_bk3633_nvds_pref_entry_t *entry =
            &name_space->provider->entries[current->next_index++];
        uint8_t scratch[H2_BK3633_NVDS_VALUE_SIZE_MAX];
        size_t len = 0u;
        h2_pal_result_t rc;
        if (strcmp(entry->name_space, name_space->name_space) != 0) {
            continue;
        }
        rc = pref_read_raw(
            name_space, entry, scratch, entry->max_value_size, &len);
        if (rc == H2_PAL_ERR_NOT_FOUND) {
            continue;
        }
        if (rc != H2_PAL_OK) {
            return rc;
        }
        *out_entry = (h2_pal_pref_entry_t){
            .key = entry->key,
            .type = entry->type,
            .value_size = len,
        };
        return H2_PAL_OK;
    }
    current->owner = NULL;
    current->next_index = 0u;
    *cursor = NULL;
    return H2_PAL_ERR_NOT_FOUND;
}

static int pref_iterate_close(
    h2_pal_pref_namespace_t *api,
    h2_pal_pref_cursor_t **cursor)
{
    h2_bk3633_nvds_pref_namespace_t *name_space;
    if (api == NULL || api->user == NULL || cursor == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (*cursor == NULL) {
        return H2_PAL_OK;
    }
    name_space = (h2_bk3633_nvds_pref_namespace_t *)api->user;
    if (*cursor != &name_space->cursor || (*cursor)->owner != name_space) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (*cursor)->owner = NULL;
    (*cursor)->next_index = 0u;
    *cursor = NULL;
    return H2_PAL_OK;
}

static void pref_handle_prepare(h2_bk3633_nvds_pref_namespace_t *name_space)
{
    name_space->api = (h2_pal_pref_namespace_t){
        .user = name_space,
        .close = pref_close,
        .get_blob = pref_get_blob,
        .set_blob = pref_set_blob,
        .get_string = pref_get_string,
        .set_string = pref_set_string,
        .get_u32 = pref_get_u32,
        .set_u32 = pref_set_u32,
        .get_i32 = pref_get_i32,
        .set_i32 = pref_set_i32,
        .get_bool = pref_get_bool,
        .set_bool = pref_set_bool,
        .remove = pref_remove,
        .clear = pref_clear,
        .commit = pref_commit,
        .iterate = pref_iterate,
        .iterate_close = pref_iterate_close,
    };
}

static int pref_open(
    void *user,
    const char *name_space_name,
    h2_pal_pref_open_mode_t mode,
    h2_pal_pref_namespace_t **out_namespace)
{
    h2_bk3633_nvds_pref_state_t *provider =
        (h2_bk3633_nvds_pref_state_t *)user;
    const char *declared_name_space = NULL;
    size_t index;
    if (provider == NULL || !provider->ready || name_space_name == NULL ||
        name_space_name[0] == '\0' || out_namespace == NULL ||
        (mode != H2_PAL_PREF_OPEN_READ_ONLY &&
         mode != H2_PAL_PREF_OPEN_READ_WRITE)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_namespace = NULL;
    for (index = 0u; index < provider->entry_count; ++index) {
        if (strcmp(provider->entries[index].name_space, name_space_name) == 0) {
            declared_name_space = provider->entries[index].name_space;
            break;
        }
    }
    if (declared_name_space == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    for (index = 0u; index < H2_BK3633_NVDS_PREF_HANDLE_COUNT; ++index) {
        h2_bk3633_nvds_pref_namespace_t *name_space =
            &provider->handles[index];
        if (name_space->active) {
            continue;
        }
        pref_handle_prepare(name_space);
        name_space->provider = provider;
        name_space->name_space = declared_name_space;
        name_space->mode = mode;
        name_space->cursor.owner = NULL;
        name_space->cursor.next_index = 0u;
        name_space->active = true;
        *out_namespace = &name_space->api;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_FULL;
}

static const h2_pal_pref_vtable_t s_pref_vtable = {
    .open = pref_open,
};

h2_pal_result_t h2_bk3633_nvds_pref_init_with_driver(
    const h2_bk3633_nvds_pref_entry_t *entries,
    size_t entry_count,
    const h2_bk3633_nvds_driver_t *driver)
{
    h2_pal_result_t rc;
    if (s_nvds_pref.ready) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (driver == NULL || driver->get == NULL || driver->put == NULL ||
        driver->del == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = pref_mapping_validate(entries, entry_count);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memset(&s_nvds_pref, 0, sizeof(s_nvds_pref));
    s_nvds_pref.entries = entries;
    s_nvds_pref.entry_count = entry_count;
    s_nvds_pref.driver = *driver;
    s_nvds_pref.api.user = &s_nvds_pref;
    s_nvds_pref.api.vtable = &s_pref_vtable;
    s_nvds_pref.ready = true;
    return H2_PAL_OK;
}

#if defined(BK3633)
static h2_bk3633_nvds_status_t sdk_nvds_status(uint8_t status)
{
    switch (status) {
    case NVDS_OK: return H2_BK3633_NVDS_STATUS_OK;
    case NVDS_TAG_NOT_DEFINED: return H2_BK3633_NVDS_STATUS_NOT_FOUND;
    case NVDS_NO_SPACE_AVAILABLE: return H2_BK3633_NVDS_STATUS_NO_SPACE;
    case NVDS_LENGTH_OUT_OF_RANGE: return H2_BK3633_NVDS_STATUS_LENGTH;
    case NVDS_PARAM_LOCKED: return H2_BK3633_NVDS_STATUS_LOCKED;
    case NVDS_CORRUPT: return H2_BK3633_NVDS_STATUS_CORRUPT;
    default: return H2_BK3633_NVDS_STATUS_FAIL;
    }
}

static h2_bk3633_nvds_status_t sdk_nvds_get(
    void *user,
    uint8_t tag,
    uint8_t *in_out_len,
    uint8_t *data)
{
    (void)user;
    return sdk_nvds_status(nvds_get(tag, in_out_len, data));
}

static h2_bk3633_nvds_status_t sdk_nvds_put(
    void *user,
    uint8_t tag,
    uint8_t len,
    const uint8_t *data)
{
    (void)user;
    return sdk_nvds_status(nvds_put(tag, len, (uint8_t *)data));
}

static h2_bk3633_nvds_status_t sdk_nvds_del(void *user, uint8_t tag)
{
    (void)user;
    return sdk_nvds_status(nvds_del(tag));
}
#endif

h2_pal_result_t h2_bk3633_nvds_pref_init(
    const h2_bk3633_nvds_pref_entry_t *entries,
    size_t entry_count)
{
#if defined(BK3633)
    static const h2_bk3633_nvds_driver_t driver = {
        .get = sdk_nvds_get,
        .put = sdk_nvds_put,
        .del = sdk_nvds_del,
        .user = NULL,
    };
    return h2_bk3633_nvds_pref_init_with_driver(entries, entry_count, &driver);
#else
    (void)entries;
    (void)entry_count;
    return H2_PAL_ERR_UNAVAILABLE;
#endif
}

void h2_bk3633_nvds_pref_deinit(void)
{
    memset(&s_nvds_pref, 0, sizeof(s_nvds_pref));
}

const h2_pal_pref_api_t *h2_bk3633_nvds_pref_api(void)
{
    return s_nvds_pref.ready ?
        &s_nvds_pref.api : h2_pal_unsupported_pref_api();
}
