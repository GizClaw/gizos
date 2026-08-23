#include "h2_pixa_platform.h"

#include "pixa_extract.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_ENTRY_COUNT 16u
#define FAKE_HANDLE_COUNT 8u
#define FAKE_PATH_CAPACITY 128u
#define FAKE_DATA_CAPACITY 1024u

typedef struct fake_entry {
    char path[FAKE_PATH_CAPACITY];
    uint8_t data[FAKE_DATA_CAPACITY];
    size_t len;
    int is_dir;
    int exists;
} fake_entry_t;

struct h2_pal_fs_file {
    fake_entry_t *entry;
    size_t position;
    int in_use;
};

typedef struct fake_platform {
    fake_entry_t entries[FAKE_ENTRY_COUNT];
    h2_pal_fs_file_t handles[FAKE_HANDLE_COUNT];
    h2_pal_fs_api_t fs_api;
    h2_pal_mem_api_t mem_api;
    int force_result;
    int forced_result;
    int allocation_fails;
    size_t allocation_count;
    size_t free_count;
} fake_platform_t;

static int fake_result(fake_platform_t *fake) {
    return fake->force_result ? fake->forced_result : H2_PAL_OK;
}

static fake_entry_t *fake_find(fake_platform_t *fake, const char *path) {
    for (size_t index = 0u; index < FAKE_ENTRY_COUNT; ++index) {
        if (fake->entries[index].exists &&
            strcmp(fake->entries[index].path, path) == 0) {
            return &fake->entries[index];
        }
    }
    return NULL;
}

static fake_entry_t *fake_create(
    fake_platform_t *fake,
    const char *path,
    int is_dir) {
    fake_entry_t *existing = fake_find(fake, path);
    if (existing != NULL) {
        existing->is_dir = is_dir;
        return existing;
    }
    for (size_t index = 0u; index < FAKE_ENTRY_COUNT; ++index) {
        fake_entry_t *entry = &fake->entries[index];
        if (!entry->exists) {
            int length = snprintf(entry->path, sizeof(entry->path), "%s", path);
            if (length < 0 || (size_t)length >= sizeof(entry->path)) {
                return NULL;
            }
            entry->exists = 1;
            entry->is_dir = is_dir;
            entry->len = 0u;
            return entry;
        }
    }
    return NULL;
}

static h2_pal_fs_file_t *fake_handle(fake_platform_t *fake) {
    for (size_t index = 0u; index < FAKE_HANDLE_COUNT; ++index) {
        if (!fake->handles[index].in_use) {
            fake->handles[index].in_use = 1;
            fake->handles[index].position = 0u;
            return &fake->handles[index];
        }
    }
    return NULL;
}

static int fake_mkdir(void *user, const char *path) {
    fake_platform_t *fake = (fake_platform_t *)user;
    int result = fake_result(fake);
    if (result != H2_PAL_OK) {
        return result;
    }
    return fake_create(fake, path, 1) != NULL
               ? H2_PAL_OK
               : H2_PAL_ERR_NO_SPACE;
}

static int fake_open(
    void *user,
    const char *path,
    h2_pal_fs_open_mode_t mode,
    h2_pal_fs_file_t **out_file) {
    fake_platform_t *fake = (fake_platform_t *)user;
    fake_entry_t *entry;
    h2_pal_fs_file_t *handle;
    int result = fake_result(fake);

    *out_file = NULL;
    if (result != H2_PAL_OK) {
        return result;
    }
    entry = fake_find(fake, path);
    if (mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE) {
        if (entry == NULL) {
            entry = fake_create(fake, path, 0);
        }
        if (entry != NULL) {
            entry->len = 0u;
        }
    } else if (mode != H2_PAL_FS_OPEN_READ) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (entry == NULL || entry->is_dir) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    handle = fake_handle(fake);
    if (handle == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    handle->entry = entry;
    *out_file = handle;
    return H2_PAL_OK;
}

static int fake_read(
    void *user,
    h2_pal_fs_file_t *file,
    void *data,
    size_t len,
    size_t *out_read) {
    fake_platform_t *fake = (fake_platform_t *)user;
    size_t available;
    int result = fake_result(fake);
    if (result != H2_PAL_OK) {
        return result;
    }
    available = file->entry->len - file->position;
    if (len > available) {
        len = available;
    }
    memcpy(data, file->entry->data + file->position, len);
    file->position += len;
    *out_read = len;
    return H2_PAL_OK;
}

static int fake_seek(
    void *user,
    h2_pal_fs_file_t *file,
    uint64_t position) {
    fake_platform_t *fake = (fake_platform_t *)user;
    int result = fake_result(fake);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (position > file->entry->len) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    file->position = (size_t)position;
    return H2_PAL_OK;
}

static int fake_write(
    void *user,
    h2_pal_fs_file_t *file,
    const void *data,
    size_t len,
    size_t *out_written) {
    fake_platform_t *fake = (fake_platform_t *)user;
    int result = fake_result(fake);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (file->position > FAKE_DATA_CAPACITY ||
        len > FAKE_DATA_CAPACITY - file->position) {
        return H2_PAL_ERR_NO_SPACE;
    }
    memcpy(file->entry->data + file->position, data, len);
    file->position += len;
    if (file->entry->len < file->position) {
        file->entry->len = file->position;
    }
    *out_written = len;
    return H2_PAL_OK;
}

static int fake_sync(void *user, h2_pal_fs_file_t *file) {
    fake_platform_t *fake = (fake_platform_t *)user;
    (void)file;
    return fake_result(fake);
}

static int fake_close(void *user, h2_pal_fs_file_t *file) {
    fake_platform_t *fake = (fake_platform_t *)user;
    int result = fake_result(fake);
    if (result == H2_PAL_OK) {
        file->entry = NULL;
        file->position = 0u;
        file->in_use = 0;
    }
    return result;
}

static int fake_stat(
    void *user,
    const char *path,
    h2_pal_fs_stat_t *out_stat) {
    fake_platform_t *fake = (fake_platform_t *)user;
    fake_entry_t *entry;
    int result = fake_result(fake);
    if (result != H2_PAL_OK) {
        return result;
    }
    entry = fake_find(fake, path);
    if (entry == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    out_stat->size = entry->len;
    out_stat->is_dir = entry->is_dir;
    return H2_PAL_OK;
}

static int fake_clear(void *user, const char *path) {
    fake_platform_t *fake = (fake_platform_t *)user;
    int result = fake_result(fake);
    (void)path;
    return result;
}

static int fake_remove(void *user, const char *path) {
    fake_platform_t *fake = (fake_platform_t *)user;
    fake_entry_t *entry;
    int result = fake_result(fake);
    if (result != H2_PAL_OK) {
        return result;
    }
    entry = fake_find(fake, path);
    if (entry != NULL) {
        *entry = (fake_entry_t){0};
    }
    return H2_PAL_OK;
}

static int fake_rename(
    void *user,
    const char *old_path,
    const char *new_path) {
    fake_platform_t *fake = (fake_platform_t *)user;
    fake_entry_t *entry;
    int length;
    int result = fake_result(fake);
    if (result != H2_PAL_OK) {
        return result;
    }
    entry = fake_find(fake, old_path);
    if (entry == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    length = snprintf(entry->path, sizeof(entry->path), "%s", new_path);
    return length >= 0 && (size_t)length < sizeof(entry->path)
               ? H2_PAL_OK
               : H2_PAL_ERR_NO_SPACE;
}

static void *fake_alloc(void *user, size_t len) {
    fake_platform_t *fake = (fake_platform_t *)user;
    if (fake->allocation_fails) {
        return NULL;
    }
    ++fake->allocation_count;
    return malloc(len);
}

static void fake_free(void *user, void *ptr) {
    fake_platform_t *fake = (fake_platform_t *)user;
    ++fake->free_count;
    free(ptr);
}

static const h2_pal_fs_vtable_t fake_fs_vtable = {
    .mkdir = fake_mkdir,
    .open = fake_open,
    .read = fake_read,
    .seek = fake_seek,
    .write = fake_write,
    .sync = fake_sync,
    .close = fake_close,
    .stat = fake_stat,
    .clear = fake_clear,
    .remove = fake_remove,
    .rename = fake_rename,
};

static const h2_pal_mem_vtable_t fake_mem_vtable = {
    .alloc = fake_alloc,
    .realloc = NULL,
    .free = fake_free,
};

static void init_platform(
    fake_platform_t *fake,
    const h2_pal_fs_vtable_t *fs_vtable,
    h2_pixa_platform_t *platform) {
    h2_pixa_platform_config_t config;
    fake->fs_api = (h2_pal_fs_api_t){.user = fake, .vtable = fs_vtable};
    fake->mem_api =
        (h2_pal_mem_api_t){.user = fake, .vtable = &fake_mem_vtable};
    config.fs = &fake->fs_api;
    config.mem = &fake->mem_api;
    assert(h2_pixa_platform_init(platform, &config) == H2_PAL_OK);
}

static void put_u16(uint8_t *data, size_t offset, uint16_t value) {
    data[offset] = (uint8_t)value;
    data[offset + 1u] = (uint8_t)(value >> 8u);
}

static void put_u32(uint8_t *data, size_t offset, uint32_t value) {
    put_u16(data, offset, (uint16_t)value);
    put_u16(data, offset + 2u, (uint16_t)(value >> 16u));
}

static size_t sample_pixa(uint8_t *data, size_t capacity) {
    const size_t header_offset = 40u;
    const size_t palette_offset = header_offset;
    const size_t clip_offset = palette_offset + 2u;
    const size_t frame_offset = clip_offset + 56u;
    const size_t payload_offset = frame_offset + 16u;
    const size_t total_len = payload_offset + 4u;

    assert(capacity >= total_len);
    memset(data, 0, capacity);
    memcpy(data, "PIXA", 4u);
    put_u16(data, 4u, 1u);
    put_u16(data, 6u, (uint16_t)header_offset);
    put_u16(data, 8u, 2u);
    put_u16(data, 10u, 1u);
    put_u16(data, 12u, 1u);
    put_u16(data, 14u, 1u);
    put_u32(data, 16u, 1u);
    put_u32(data, 20u, (uint32_t)palette_offset);
    put_u32(data, 24u, (uint32_t)clip_offset);
    put_u32(data, 28u, (uint32_t)frame_offset);
    put_u32(data, 32u, (uint32_t)payload_offset);
    put_u32(data, 36u, 4u);
    memcpy(data + clip_offset, "idle", 4u);
    put_u32(data, clip_offset + 40u, 1u);
    put_u32(data, clip_offset + 44u, 100u);
    put_u16(data, clip_offset + 48u, 1u);
    put_u16(data, frame_offset, 100u);
    put_u32(data, frame_offset + 8u, 4u);
    data[payload_offset] = 0x00u;
    data[payload_offset + 1u] = 0xf8u;
    data[payload_offset + 2u] = 0x1fu;
    data[payload_offset + 3u] = 0x00u;
    return total_len;
}

static void test_init_and_operations(void) {
    fake_platform_t fake = {0};
    h2_pixa_platform_t platform = {0};
    init_platform(&fake, &fake_fs_vtable, &platform);
    const pixa_osal_api_t *osal = h2_pixa_platform_osal(&platform);
    const pixa_alloc_t *allocator = h2_pixa_platform_allocator(&platform);
    pixa_osal_file_t *file = NULL;
    pixa_osal_stat_t stat = {0};
    const uint8_t written[] = {0x11u, 0x22u};
    uint8_t read = 0u;
    size_t count = 0u;
    void *allocation;

    assert(osal != NULL);
    assert(allocator != NULL);
    assert(pixa_osal_mkdir(osal, "dir") == PIXA_OSAL_OK);
    assert(pixa_osal_open(
               osal, "dir/file", PIXA_OSAL_OPEN_WRITE_TRUNCATE, &file) ==
           PIXA_OSAL_OK);
    assert(pixa_osal_write(osal, file, written, sizeof(written), &count) ==
           PIXA_OSAL_OK);
    assert(count == sizeof(written));
    assert(pixa_osal_sync(osal, file) == PIXA_OSAL_OK);
    assert(pixa_osal_close(osal, file) == PIXA_OSAL_OK);
    assert(pixa_osal_stat(osal, "dir/file", &stat) == PIXA_OSAL_OK);
    assert(stat.size == sizeof(written) && !stat.is_dir);
    assert(pixa_osal_open(osal, "dir/file", PIXA_OSAL_OPEN_READ, &file) ==
           PIXA_OSAL_OK);
    assert(pixa_osal_seek(osal, file, 1u) == PIXA_OSAL_OK);
    assert(pixa_osal_read(osal, file, &read, 1u, &count) == PIXA_OSAL_OK);
    assert(count == 1u && read == 0x22u);
    assert(pixa_osal_close(osal, file) == PIXA_OSAL_OK);
    assert(osal->vtable->clear(osal->user, "dir") == PIXA_OSAL_OK);
    assert(pixa_osal_rename(osal, "dir/file", "dir/renamed") == PIXA_OSAL_OK);
    assert(pixa_osal_remove(osal, "dir/renamed") == PIXA_OSAL_OK);

    allocation = allocator->alloc(allocator->user, 8u);
    assert(allocation != NULL);
    allocator->free(allocator->user, allocation);
    assert(fake.allocation_count == 1u && fake.free_count == 1u);

    h2_pixa_platform_deinit(&platform);
    assert(h2_pixa_platform_osal(&platform) == NULL);
    assert(h2_pixa_platform_allocator(&platform) == NULL);
    h2_pixa_platform_deinit(NULL);
}

static void test_deinit_does_not_close_caller_file(void) {
    fake_platform_t fake = {0};
    h2_pixa_platform_t platform = {0};
    init_platform(&fake, &fake_fs_vtable, &platform);
    const pixa_osal_api_t *osal = h2_pixa_platform_osal(&platform);
    pixa_osal_file_t *file = NULL;

    assert(pixa_osal_open(
               osal, "borrowed", PIXA_OSAL_OPEN_WRITE_TRUNCATE, &file) ==
           PIXA_OSAL_OK);
    h2_pal_fs_file_t *pal_file = (h2_pal_fs_file_t *)file;
    assert(pal_file->in_use);

    h2_pixa_platform_deinit(&platform);
    assert(pal_file->in_use);
    assert(h2_pal_fs_close(&fake.fs_api, pal_file) == H2_PAL_OK);
}

static void test_error_mapping_and_optional_sync(void) {
    static const struct {
        int pal;
        int pixa;
    } mappings[] = {
        {H2_PAL_ERR_INVALID_ARG, PIXA_OSAL_ERR_INVALID_ARG},
        {H2_PAL_ERR_IO, PIXA_OSAL_ERR_IO},
        {H2_PAL_ERR_NO_MEMORY, PIXA_OSAL_ERR_NO_MEMORY},
        {H2_PAL_ERR_NO_SPACE, PIXA_OSAL_ERR_NO_SPACE},
        {H2_PAL_ERR_UNSUPPORTED, PIXA_OSAL_ERR_UNSUPPORTED},
        {H2_PAL_ERR_TIMEOUT, PIXA_OSAL_ERR_IO},
    };
    fake_platform_t fake = {0};
    h2_pixa_platform_t platform = {0};
    init_platform(&fake, &fake_fs_vtable, &platform);
    const pixa_osal_api_t *osal = h2_pixa_platform_osal(&platform);

    fake.force_result = 1;
    for (size_t index = 0u; index < sizeof(mappings) / sizeof(mappings[0]);
         ++index) {
        fake.forced_result = mappings[index].pal;
        assert(pixa_osal_mkdir(osal, "mapped") == mappings[index].pixa);
    }
    fake.force_result = 0;
    h2_pixa_platform_deinit(&platform);

    {
        h2_pal_fs_vtable_t no_sync = fake_fs_vtable;
        pixa_osal_file_t *file = NULL;
        no_sync.sync = NULL;
        init_platform(&fake, &no_sync, &platform);
        osal = h2_pixa_platform_osal(&platform);
        assert(pixa_osal_open(
                   osal, "sync", PIXA_OSAL_OPEN_WRITE_TRUNCATE, &file) ==
               PIXA_OSAL_OK);
        assert(pixa_osal_sync(osal, file) == PIXA_OSAL_OK);
        assert(pixa_osal_close(osal, file) == PIXA_OSAL_OK);
        h2_pixa_platform_deinit(&platform);
    }
}

static void test_extract_and_allocator_failure(void) {
    fake_platform_t fake = {0};
    h2_pixa_platform_t platform = {0};
    init_platform(&fake, &fake_fs_vtable, &platform);
    uint8_t data[128];
    size_t data_len = sample_pixa(data, sizeof(data));
    pixa_extract_stats_t stats = {0};

    assert(pixa_extract_memory_to_dir(
               data,
               data_len,
               "sample",
               h2_pixa_platform_osal(&platform),
               h2_pixa_platform_allocator(&platform),
               &stats) == PIXA_OK);
    assert(stats.frame_count == 1u);
    assert(fake_find(&fake, "sample/index.bin") != NULL);
    assert(fake_find(&fake, "sample/clips/idle.argb4444") != NULL);
    assert(fake.allocation_count == fake.free_count);

    fake.allocation_fails = 1;
    assert(pixa_extract_memory_to_dir(
               data,
               data_len,
               "failed",
               h2_pixa_platform_osal(&platform),
               h2_pixa_platform_allocator(&platform),
               NULL) == PIXA_ERR_NO_MEMORY);
    h2_pixa_platform_deinit(&platform);
}

static void test_invalid_config(void) {
    h2_pixa_platform_t platform = {0};
    h2_pixa_platform_config_t config = {0};
    fake_platform_t fake = {0};
    const h2_pal_mem_api_t mem = {.user = &fake, .vtable = &fake_mem_vtable};
    const h2_pal_fs_api_t fs = {.user = &fake, .vtable = &fake_fs_vtable};

    assert(h2_pixa_platform_init(NULL, &config) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pixa_platform_init(&platform, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pixa_platform_init(&platform, &config) ==
           H2_PAL_ERR_INVALID_ARG);
    config.fs = &fs;
    assert(h2_pixa_platform_init(&platform, &config) ==
           H2_PAL_ERR_INVALID_ARG);
    config.mem = &mem;
    assert(h2_pixa_platform_init(&platform, &config) == H2_PAL_OK);
    h2_pixa_platform_deinit(&platform);
}

int main(void) {
    test_invalid_config();
    test_init_and_operations();
    test_deinit_does_not_close_caller_file();
    test_error_mapping_and_optional_sync();
    test_extract_and_allocator_failure();
    return 0;
}
