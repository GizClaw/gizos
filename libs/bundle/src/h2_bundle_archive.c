#include "h2_bundle_archive.h"

#include "h2_bundle_manifest.h"
#include "h2_bundle_path.h"
#include "h2_bundle_tar.h"
#include "h2_pixa_platform.h"
#include "pixa_extract.h"

#include <stdint.h>
#include <string.h>
#include <zlib.h>

#define H2_BUNDLE_IO_BUF_SIZE 4096u

typedef enum tar_state_kind {
    TAR_STATE_HEADER = 1,
    TAR_STATE_FILE = 2,
    TAR_STATE_PADDING = 3,
} tar_state_kind_t;

typedef enum tar_output_kind {
    TAR_OUTPUT_NONE = 0,
    TAR_OUTPUT_FS = 1,
    TAR_OUTPUT_APP = 2,
    TAR_OUTPUT_CHECKSUM = 3,
    TAR_OUTPUT_SKIP = 4,
} tar_output_kind_t;

typedef struct tar_state {
    h2_bundle_installer_t *installer;
    const h2_bundle_install_options_t *options;
    uint8_t *manifest_seen;
    tar_state_kind_t kind;
    tar_output_kind_t output;
    h2_bundle_entry_t entry;
    h2_bundle_entry_t app_entry;
    uint8_t header[512];
    size_t header_len;
    uint64_t remaining;
    uint64_t padding_remaining;
    int zero_blocks;
    int done;
    h2_pal_fs_file_t *out_file;
    uint8_t *pixa_data;
    size_t pixa_len;
    size_t pixa_cap;
    uint8_t checksum_data[128];
    size_t checksum_len;
    int checksum_seen;
    int package_manifest_seen;
    int app_seen;
    int app_writer_active;
    int data_install_needed;
    int data_cleared;
    uint32_t entry_crc32;
    char dst_path[H2_BUNDLE_PATH_MAX];
} tar_state_t;

typedef struct pixa_progress_context {
    tar_state_t *state;
    uint64_t reported_bytes;
} pixa_progress_context_t;

static int map_fs(int rc) {
    return rc == H2_PAL_FS_OK ? H2_BUNDLE_OK : H2_BUNDLE_ERR_FS;
}

static int map_pixa(int rc) {
    return rc == PIXA_OK ? H2_BUNDLE_OK : H2_BUNDLE_ERR_PIXA;
}

static int bundle_fs_open(const h2_pal_fs_api_t *fs, const char *path, h2_pal_fs_open_mode_t mode, h2_pal_fs_file_t **out_file) {
    if (fs == NULL || path == NULL || out_file == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_file = NULL;
    return h2_pal_fs_open(fs, path, mode, out_file);
}

static int bundle_fs_read(const h2_pal_fs_api_t *fs, h2_pal_fs_file_t *file, void *data, size_t len, size_t *out_read) {
    if (fs == NULL || file == NULL || data == NULL || out_read == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    return h2_pal_fs_read(fs, file, data, len, out_read);
}

static int bundle_fs_write_all(const h2_pal_fs_api_t *fs, h2_pal_fs_file_t *file, const void *data, size_t len) {
    const uint8_t *cursor = (const uint8_t *)data;
    size_t remaining = len;

    if (fs == NULL || file == NULL || (data == NULL && len != 0u)) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    while (remaining > 0u) {
        size_t written = 0u;
        int rc = h2_pal_fs_write(fs, file, cursor, remaining, &written);
        if (rc != H2_PAL_FS_OK) {
            return rc;
        }
        if (written == 0u || written > remaining) {
            return H2_PAL_FS_ERR_IO;
        }
        cursor += written;
        remaining -= written;
    }
    return H2_PAL_FS_OK;
}

static int bundle_fs_sync(const h2_pal_fs_api_t *fs, h2_pal_fs_file_t *file) {
    if (fs == NULL || file == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    return h2_pal_fs_sync(fs, file);
}

static int bundle_fs_close(const h2_pal_fs_api_t *fs, h2_pal_fs_file_t *file) {
    if (fs == NULL || file == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    return h2_pal_fs_close(fs, file);
}

static int bundle_fs_make_dir_all(const h2_pal_fs_api_t *fs, const char *path) {
    char current[384];
    size_t len;

    if (fs == NULL || path == NULL || path[0] == '\0') {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    len = strlen(path);
    if (len >= sizeof(current)) {
        return H2_PAL_FS_ERR_NO_SPACE;
    }
    memcpy(current, path, len + 1u);
    for (size_t i = 1u; i < len; ++i) {
        if (current[i] != '/') {
            continue;
        }
        current[i] = '\0';
        if (current[0] != '\0') {
            int rc = h2_pal_fs_mkdir(fs, current);
            if (rc != H2_PAL_FS_OK && rc != H2_PAL_FS_ERR_UNSUPPORTED) {
                return rc;
            }
        }
        current[i] = '/';
    }
    return h2_pal_fs_mkdir(fs, current);
}

static int bundle_fs_write_file(const h2_pal_fs_api_t *fs, const char *path, const void *data, size_t len) {
    h2_pal_fs_file_t *file = NULL;
    int rc = bundle_fs_open(fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
    int close_rc;

    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    rc = bundle_fs_write_all(fs, file, data, len);
    if (rc == H2_PAL_FS_OK) {
        rc = bundle_fs_sync(fs, file);
    }
    close_rc = bundle_fs_close(fs, file);
    return rc == H2_PAL_FS_OK ? close_rc : rc;
}

static int path_starts_with(const char *path, const char *prefix) {
    size_t prefix_len = strlen(prefix);
    return strncmp(path, prefix, prefix_len) == 0;
}

static int read_installed_checksum(h2_bundle_installer_t *installer, const char *path, uint8_t *out, size_t out_cap, size_t *out_len) {
    h2_pal_fs_file_t *file = NULL;
    size_t total = 0u;
    int rc;

    if (out_len == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    *out_len = 0u;
    if (path == NULL || path[0] == '\0') {
        return H2_BUNDLE_ERR_FS;
    }
    rc = bundle_fs_open(installer->fs, path, H2_PAL_FS_OPEN_READ, &file);
    if (rc == H2_PAL_FS_ERR_NOT_FOUND) {
        return H2_BUNDLE_OK;
    }
    if (rc != H2_PAL_FS_OK) {
        return H2_BUNDLE_ERR_FS;
    }
    while (total < out_cap) {
        size_t n = 0u;
        rc = bundle_fs_read(installer->fs, file, out + total, out_cap - total, &n);
        if (rc != H2_PAL_FS_OK) {
            bundle_fs_close(installer->fs, file);
            return H2_BUNDLE_ERR_FS;
        }
        if (n == 0u) {
            break;
        }
        total += n;
    }
    bundle_fs_close(installer->fs, file);
    *out_len = total;
    return H2_BUNDLE_OK;
}

static int finish_checksum_entry(tar_state_t *state) {
    uint8_t installed[sizeof(state->checksum_data)];
    size_t installed_len = 0u;
    int rc;

    if (state->checksum_seen || state->checksum_len == 0u) {
        return H2_BUNDLE_ERR_LAYOUT;
    }
    state->checksum_seen = 1;
    if (state->options->skip_data_install) {
        state->data_install_needed = 0;
        return H2_BUNDLE_OK;
    }
    rc = read_installed_checksum(state->installer,
        state->options->installed_checksum_path,
        installed,
        sizeof(installed),
        &installed_len);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    state->data_install_needed = !(installed_len == state->checksum_len &&
                                   memcmp(installed, state->checksum_data, state->checksum_len) == 0);
    return H2_BUNDLE_OK;
}

static int ensure_data_cleared(tar_state_t *state) {
    int rc;

    if (state->data_cleared || !state->data_install_needed) {
        return H2_BUNDLE_OK;
    }
    if (state->options->clear_data != NULL) {
        rc = state->options->clear_data(state->options->clear_data_user, state->options->dst_root);
        if (rc != H2_BUNDLE_OK) {
            return rc;
        }
    }
    state->data_cleared = 1;
    return H2_BUNDLE_OK;
}

static int ensure_parent_dirs(const h2_pal_fs_api_t *fs, const char *path) {
    char parent[H2_BUNDLE_PATH_MAX];
    int rc = h2_bundle_path_parent_dir(parent, sizeof(parent), path);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    if (parent[0] == '\0') {
        return H2_BUNDLE_OK;
    }
    return map_fs(bundle_fs_make_dir_all(fs, parent));
}

static voidpf zlib_alloc_adapter(voidpf opaque, uInt items, uInt size) {
    const h2_pal_mem_api_t *allocator = (const h2_pal_mem_api_t *)opaque;
    size_t total;

    if (allocator == NULL || items == 0u || size == 0u) {
        return Z_NULL;
    }
    if ((size_t)items > ((size_t)-1) / (size_t)size) {
        return Z_NULL;
    }
    total = (size_t)items * (size_t)size;
    return h2_pal_mem_alloc(allocator, total);
}

static void zlib_free_adapter(voidpf opaque, voidpf address) {
    h2_pal_mem_free((const h2_pal_mem_api_t *)opaque, address);
}

static int grow_pixa_buffer(tar_state_t *state, size_t extra_len) {
    size_t needed;
    size_t cap;
    void *next;

    if (extra_len == 0u) {
        return H2_BUNDLE_OK;
    }
    if (state->pixa_len > (size_t)-1 - extra_len) {
        return H2_BUNDLE_ERR_NO_SPACE;
    }
    needed = state->pixa_len + extra_len;
    if (needed <= state->pixa_cap) {
        return H2_BUNDLE_OK;
    }
    cap = state->pixa_cap == 0u ? 1024u : state->pixa_cap;
    while (cap < needed) {
        if (cap > ((size_t)-1 / 2u)) {
            cap = needed;
            break;
        }
        cap *= 2u;
    }
    next = h2_pal_mem_realloc(state->installer->allocator, state->pixa_data, cap);
    if (next == NULL) {
        next = h2_pal_mem_alloc(state->installer->allocator, cap);
        if (next != NULL && state->pixa_data != NULL) {
            memcpy(next, state->pixa_data, state->pixa_len);
            h2_pal_mem_free(state->installer->allocator, state->pixa_data);
        }
    }
    if (next == NULL) {
        return H2_BUNDLE_ERR_NO_MEMORY;
    }
    state->pixa_data = (uint8_t *)next;
    state->pixa_cap = cap;
    return H2_BUNDLE_OK;
}

static void free_installer_mem(h2_bundle_installer_t *installer, void *ptr) {
    h2_pal_mem_free(installer->allocator, ptr);
}

static int append_pixa_bytes(tar_state_t *state, const uint8_t *data, size_t len) {
    int rc = grow_pixa_buffer(state, len);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    memcpy(state->pixa_data + state->pixa_len, data, len);
    state->pixa_len += len;
    return H2_BUNDLE_OK;
}

static void release_entry_buffers(tar_state_t *state) {
    if (state->app_writer_active && state->options != NULL &&
        state->options->app_writer != NULL && state->options->app_writer->abort != NULL) {
        state->options->app_writer->abort(state->options->app_writer->user);
        state->app_writer_active = 0;
    }
    if (state->out_file != NULL) {
        bundle_fs_close(state->installer->fs, state->out_file);
        state->out_file = NULL;
    }
    if (state->pixa_data != NULL) {
        free_installer_mem(state->installer, state->pixa_data);
        state->pixa_data = NULL;
    }
    state->pixa_len = 0u;
    state->pixa_cap = 0u;
}

static void release_manifest_seen(tar_state_t *state) {
    if (state->manifest_seen != NULL) {
        h2_pal_mem_free(state->installer->allocator, state->manifest_seen);
        state->manifest_seen = NULL;
    }
}

static void report_progress(tar_state_t *state) {
    if (state->installer->progress != NULL) {
        state->installer->progress(state->installer->progress_user, &state->entry, &state->installer->stats);
    }
}

static void report_pixa_extract_progress(
    void *user,
    uint32_t completed_frames,
    uint32_t total_frames) {
    pixa_progress_context_t *progress = (pixa_progress_context_t *)user;
    tar_state_t *state;
    uint64_t target;
    uint64_t delta;

    if (progress == NULL || progress->state == NULL) {
        return;
    }
    state = progress->state;
    if (total_frames == 0u || completed_frames >= total_frames) {
        target = state->entry.size;
    } else {
        target = (state->entry.size / total_frames) * completed_frames +
                 ((state->entry.size % total_frames) * completed_frames) /
                     total_frames;
    }
    if (target <= progress->reported_bytes) {
        return;
    }
    delta = target - progress->reported_bytes;
    if (UINT64_MAX - state->installer->stats.payload_bytes < delta) {
        return;
    }
    progress->reported_bytes = target;
    state->installer->stats.payload_bytes += delta;
    report_progress(state);
}

static int finish_file_entry(tar_state_t *state);

static int verify_manifest_entry(tar_state_t *state) {
    const h2_bundle_manifest_t *manifest;
    const h2_bundle_manifest_entry_t *entry;
    size_t index;
    int rc;

    if (state->options == NULL || state->options->manifest == NULL) {
        return H2_BUNDLE_OK;
    }
    manifest = state->options->manifest;
    entry = h2_bundle_manifest_find(manifest, state->entry.path);
    if (entry == NULL) {
        return H2_BUNDLE_ERR_MANIFEST;
    }
    rc = h2_bundle_manifest_verify_crc32(entry, state->entry.size, state->entry_crc32);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    index = (size_t)(entry - manifest->entries);
    if (state->manifest_seen != NULL) {
        if (state->manifest_seen[index] != 0u) {
            return H2_BUNDLE_ERR_MANIFEST;
        }
        state->manifest_seen[index] = 1u;
    }
    return H2_BUNDLE_OK;
}

static int start_entry(tar_state_t *state, const h2_bundle_entry_t *entry) {
    const char *dst_relative_path = entry->path;
    int rc;

    state->entry = *entry;
    state->app_entry = *entry;
    state->output = TAR_OUTPUT_NONE;
    state->remaining = entry->size;
    state->padding_remaining = h2_bundle_tar_padding(entry->size);
    state->pixa_len = 0u;
    state->pixa_cap = 0u;
    state->pixa_data = NULL;
    state->out_file = NULL;
    state->entry_crc32 = (uint32_t)crc32(0L, Z_NULL, 0);

    if (state->options->ota_layout) {
        if (strcmp(entry->path, "manifest") == 0) {
            if (state->package_manifest_seen || state->checksum_seen ||
                entry->kind != H2_BUNDLE_ENTRY_FILE || entry->size == 0u) {
                return H2_BUNDLE_ERR_LAYOUT;
            }
            state->package_manifest_seen = 1;
            state->output = TAR_OUTPUT_SKIP;
            state->kind = TAR_STATE_FILE;
            return H2_BUNDLE_OK;
        }
        if (strcmp(entry->path, "checksum") == 0) {
            if (entry->kind != H2_BUNDLE_ENTRY_FILE || entry->size == 0u || entry->size >= sizeof(state->checksum_data)) {
                return H2_BUNDLE_ERR_LAYOUT;
            }
            state->output = TAR_OUTPUT_CHECKSUM;
            state->kind = TAR_STATE_FILE;
            return H2_BUNDLE_OK;
        }
        if ((strcmp(entry->path, "data") == 0 || strcmp(entry->path, "app") == 0) &&
            entry->kind == H2_BUNDLE_ENTRY_DIR) {
            state->output = TAR_OUTPUT_SKIP;
            state->kind = TAR_STATE_HEADER;
            return H2_BUNDLE_OK;
        }
        if (path_starts_with(entry->path, "data/")) {
            if (!state->checksum_seen) {
                return H2_BUNDLE_ERR_LAYOUT;
            }
            if (!state->data_install_needed) {
                state->output = TAR_OUTPUT_SKIP;
                state->kind = entry->size == 0u ? TAR_STATE_HEADER : TAR_STATE_FILE;
                return entry->size == 0u ? H2_BUNDLE_OK : H2_BUNDLE_OK;
            }
            rc = ensure_data_cleared(state);
            if (rc != H2_BUNDLE_OK) {
                return rc;
            }
            dst_relative_path = entry->path + strlen("data/");
            if (dst_relative_path[0] == '\0') {
                if (entry->kind != H2_BUNDLE_ENTRY_DIR) {
                    return H2_BUNDLE_ERR_LAYOUT;
                }
                state->output = TAR_OUTPUT_SKIP;
                state->kind = TAR_STATE_HEADER;
                return H2_BUNDLE_OK;
            }
            state->entry.size = entry->size;
            state->entry.kind = entry->kind;
            if (strlen(dst_relative_path) >= sizeof(state->entry.path)) {
                return H2_BUNDLE_ERR_NO_SPACE;
            }
            memcpy(state->entry.path, dst_relative_path, strlen(dst_relative_path) + 1u);
        } else if (path_starts_with(entry->path, "app/")) {
            if (!state->checksum_seen) {
                return H2_BUNDLE_ERR_LAYOUT;
            }
            if (entry->kind == H2_BUNDLE_ENTRY_DIR) {
                state->output = TAR_OUTPUT_SKIP;
                state->kind = TAR_STATE_HEADER;
                return H2_BUNDLE_OK;
            }
            if (entry->kind != H2_BUNDLE_ENTRY_FILE) {
                return H2_BUNDLE_ERR_LAYOUT;
            }
            if (state->app_seen) {
                return H2_BUNDLE_ERR_LAYOUT;
            }
            state->app_seen = 1;
            if (state->options->skip_app_install) {
                state->output = TAR_OUTPUT_SKIP;
                state->kind = entry->size == 0u ? TAR_STATE_HEADER : TAR_STATE_FILE;
                return H2_BUNDLE_OK;
            }
            if (state->options->app_writer == NULL ||
                state->options->app_writer->begin == NULL ||
                state->options->app_writer->write == NULL ||
                state->options->app_writer->end == NULL) {
                return H2_BUNDLE_ERR_LAYOUT;
            }
            state->output = TAR_OUTPUT_APP;
            rc = state->options->app_writer->begin(state->options->app_writer->user, &state->app_entry);
            if (rc != H2_BUNDLE_OK) {
                return rc;
            }
            state->app_writer_active = 1;
            if (entry->size == 0u) {
                return finish_file_entry(state);
            }
            state->kind = TAR_STATE_FILE;
            return H2_BUNDLE_OK;
        } else {
            return H2_BUNDLE_ERR_LAYOUT;
        }
    }

    state->installer->stats.entry_count += 1u;
    rc = h2_bundle_path_join(state->dst_path, sizeof(state->dst_path), state->options->dst_root, dst_relative_path);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    if (entry->kind == H2_BUNDLE_ENTRY_DIR) {
        rc = map_fs(bundle_fs_make_dir_all(state->installer->fs, state->dst_path));
        if (rc != H2_BUNDLE_OK) {
            return rc;
        }
        state->installer->stats.dir_count += 1u;
        report_progress(state);
        state->kind = TAR_STATE_HEADER;
        return H2_BUNDLE_OK;
    }

    if (entry->kind != H2_BUNDLE_ENTRY_FILE) {
        return H2_BUNDLE_ERR_UNSUPPORTED_ENTRY;
    }
    state->output = TAR_OUTPUT_FS;
    rc = ensure_parent_dirs(state->installer->fs, state->dst_path);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    if (!h2_bundle_path_has_suffix(entry->path, ".pixa")) {
        rc = map_fs(bundle_fs_open(state->installer->fs, state->dst_path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &state->out_file));
        if (rc != H2_BUNDLE_OK) {
            return rc;
        }
    }
    if (entry->size == 0u) {
        return finish_file_entry(state);
    }
    state->kind = TAR_STATE_FILE;
    return H2_BUNDLE_OK;
}

static int finish_file_entry(tar_state_t *state) {
    int rc = H2_BUNDLE_OK;

    if (state->output == TAR_OUTPUT_CHECKSUM) {
        rc = finish_checksum_entry(state);
        if (rc == H2_BUNDLE_OK) {
            state->installer->stats.file_count += 1u;
            report_progress(state);
        }
        state->kind = state->padding_remaining > 0u ? TAR_STATE_PADDING : TAR_STATE_HEADER;
        return rc;
    }

    if (state->output == TAR_OUTPUT_APP) {
        rc = state->options->app_writer->end(state->options->app_writer->user, &state->app_entry);
        if (rc != H2_BUNDLE_OK && state->options->app_writer->abort != NULL) {
            state->options->app_writer->abort(state->options->app_writer->user);
        }
        state->app_writer_active = 0;
        if (rc == H2_BUNDLE_OK) {
            state->installer->stats.file_count += 1u;
            report_progress(state);
        }
        state->kind = state->padding_remaining > 0u ? TAR_STATE_PADDING : TAR_STATE_HEADER;
        return rc;
    }

    if (state->output == TAR_OUTPUT_SKIP) {
        state->kind = state->padding_remaining > 0u ? TAR_STATE_PADDING : TAR_STATE_HEADER;
        return H2_BUNDLE_OK;
    }

    rc = verify_manifest_entry(state);
    if (rc != H2_BUNDLE_OK) {
        release_entry_buffers(state);
        return rc;
    }

    if (h2_bundle_path_has_suffix(state->entry.path, ".pixa")) {
        h2_pixa_platform_t platform = {0};
        const h2_pixa_platform_config_t config = {
            .fs = state->installer->fs,
            .mem = state->installer->allocator,
        };
        pixa_extract_stats_t pixa_stats;
        pixa_progress_context_t progress = {
            .state = state,
        };
        rc = h2_pixa_platform_init(&platform, &config);
        if (rc == H2_PAL_OK) {
            rc = map_pixa(pixa_extract_memory_with_progress(
                state->pixa_data, state->pixa_len, state->dst_path,
                h2_pixa_platform_osal(&platform),
                h2_pixa_platform_allocator(&platform), &pixa_stats,
                report_pixa_extract_progress, &progress));
        } else {
            rc = H2_BUNDLE_ERR_PIXA;
        }
        h2_pixa_platform_deinit(&platform);
        if (rc == H2_BUNDLE_OK) {
            state->installer->stats.pixa_count += 1u;
        }
    } else if (state->out_file != NULL) {
        rc = map_fs(bundle_fs_sync(state->installer->fs, state->out_file));
        if (rc == H2_BUNDLE_OK) {
            rc = map_fs(bundle_fs_close(state->installer->fs, state->out_file));
            state->out_file = NULL;
        }
    }

    if (rc == H2_BUNDLE_OK) {
        state->installer->stats.file_count += 1u;
        report_progress(state);
    }
    if (state->pixa_data != NULL) {
        free_installer_mem(state->installer, state->pixa_data);
        state->pixa_data = NULL;
    }
    state->pixa_len = 0u;
    state->pixa_cap = 0u;
    if (state->padding_remaining > 0u) {
        state->kind = TAR_STATE_PADDING;
    } else {
        state->kind = TAR_STATE_HEADER;
    }
    return rc;
}

static int consume_header_bytes(tar_state_t *state, const uint8_t **cursor, size_t *remaining) {
    size_t want = 512u - state->header_len;
    h2_bundle_entry_t entry;
    int header_result = 0;
    int rc;

    if (want > *remaining) {
        want = *remaining;
    }
    memcpy(state->header + state->header_len, *cursor, want);
    state->header_len += want;
    *cursor += want;
    *remaining -= want;

    if (state->header_len < 512u) {
        return H2_BUNDLE_OK;
    }

    state->header_len = 0u;
    rc = h2_bundle_tar_parse_header(state->header, &entry, &header_result);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    if (header_result == H2_BUNDLE_TAR_HEADER_ZERO) {
        state->zero_blocks += 1;
        if (state->zero_blocks >= 2) {
            state->done = 1;
        }
        return H2_BUNDLE_OK;
    }
    state->zero_blocks = 0;
    return start_entry(state, &entry);
}

static int consume_file_bytes(tar_state_t *state, const uint8_t **cursor, size_t *remaining) {
    size_t take = *remaining;
    int rc;

    if ((uint64_t)take > state->remaining) {
        take = (size_t)state->remaining;
    }
    if (take == 0u) {
        return finish_file_entry(state);
    }

    if (state->output == TAR_OUTPUT_CHECKSUM) {
        if (state->checksum_len + take >= sizeof(state->checksum_data)) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        memcpy(state->checksum_data + state->checksum_len, *cursor, take);
        state->checksum_len += take;
        rc = H2_BUNDLE_OK;
    } else if (state->output == TAR_OUTPUT_APP) {
        rc = state->options->app_writer->write(state->options->app_writer->user, &state->app_entry, *cursor, take);
    } else if (state->output == TAR_OUTPUT_SKIP) {
        rc = H2_BUNDLE_OK;
    } else if (h2_bundle_path_has_suffix(state->entry.path, ".pixa")) {
        rc = append_pixa_bytes(state, *cursor, take);
    } else {
        rc = map_fs(bundle_fs_write_all(state->installer->fs, state->out_file, *cursor, take));
    }
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }

    state->entry_crc32 = (uint32_t)crc32(state->entry_crc32, *cursor, (uInt)take);
    if (state->output != TAR_OUTPUT_SKIP &&
        !h2_bundle_path_has_suffix(state->entry.path, ".pixa")) {
        if (UINT64_MAX - state->installer->stats.payload_bytes < take) {
            return H2_BUNDLE_ERR_NO_SPACE;
        }
        state->installer->stats.payload_bytes += take;
        report_progress(state);
    }
    state->remaining -= take;
    *cursor += take;
    *remaining -= take;
    if (state->remaining == 0u) {
        return finish_file_entry(state);
    }
    return H2_BUNDLE_OK;
}

static int consume_padding_bytes(tar_state_t *state, const uint8_t **cursor, size_t *remaining) {
    size_t take = *remaining;
    if ((uint64_t)take > state->padding_remaining) {
        take = (size_t)state->padding_remaining;
    }
    state->padding_remaining -= take;
    *cursor += take;
    *remaining -= take;
    if (state->padding_remaining == 0u) {
        state->kind = TAR_STATE_HEADER;
    }
    return H2_BUNDLE_OK;
}

static int tar_state_feed(tar_state_t *state, const uint8_t *data, size_t len) {
    const uint8_t *cursor = data;
    size_t remaining = len;
    int rc;

    if (state == NULL || (data == NULL && len != 0u)) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    while (remaining > 0u && !state->done) {
        if (state->kind == TAR_STATE_HEADER) {
            rc = consume_header_bytes(state, &cursor, &remaining);
        } else if (state->kind == TAR_STATE_FILE) {
            rc = consume_file_bytes(state, &cursor, &remaining);
        } else if (state->kind == TAR_STATE_PADDING) {
            rc = consume_padding_bytes(state, &cursor, &remaining);
        } else {
            rc = H2_BUNDLE_ERR_TAR;
        }
        if (rc != H2_BUNDLE_OK) {
            release_entry_buffers(state);
            return rc;
        }
    }
    return H2_BUNDLE_OK;
}

static int tar_state_finish(tar_state_t *state) {
    int rc;

    if (state->out_file != NULL || state->pixa_data != NULL) {
        release_entry_buffers(state);
        return H2_BUNDLE_ERR_TAR;
    }
    if (state->kind != TAR_STATE_HEADER || state->header_len != 0u) {
        return H2_BUNDLE_ERR_TAR;
    }
    if (state->zero_blocks == 0) {
        return H2_BUNDLE_ERR_TAR;
    }
    if (state->options != NULL && state->options->ota_layout) {
        if (!state->checksum_seen || !state->app_seen ||
            (!state->options->skip_data_install && state->options->installed_checksum_path == NULL)) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        if (!state->options->skip_data_install && state->data_install_needed) {
            rc = ensure_data_cleared(state);
            if (rc != H2_BUNDLE_OK) {
                return rc;
            }
            rc = bundle_fs_write_file(state->installer->fs,
                state->options->installed_checksum_path,
                state->checksum_data,
                state->checksum_len);
            if (rc != H2_PAL_FS_OK) {
                return H2_BUNDLE_ERR_FS;
            }
        }
    }
    if (state->options != NULL && state->options->manifest != NULL && state->manifest_seen != NULL) {
        const h2_bundle_manifest_t *manifest = state->options->manifest;
        for (size_t i = 0u; i < manifest->entry_count; ++i) {
            if (state->manifest_seen[i] == 0u) {
                return H2_BUNDLE_ERR_MANIFEST;
            }
        }
    }
    return H2_BUNDLE_OK;
}

static int init_manifest_seen(tar_state_t *tar) {
    const h2_bundle_manifest_t *manifest;

    if (tar->options == NULL || tar->options->manifest == NULL) {
        return H2_BUNDLE_OK;
    }
    manifest = tar->options->manifest;
    if (manifest->entry_count == 0u) {
        return H2_BUNDLE_OK;
    }
    tar->manifest_seen = (uint8_t *)h2_pal_mem_alloc(tar->installer->allocator, manifest->entry_count);
    if (tar->manifest_seen == NULL) {
        return H2_BUNDLE_ERR_NO_MEMORY;
    }
    memset(tar->manifest_seen, 0, manifest->entry_count);
    return H2_BUNDLE_OK;
}

int h2_bundle_archive_install_zlib_tar(h2_bundle_installer_t *installer, const h2_bundle_install_options_t *options) {
    h2_pal_fs_file_t *archive = NULL;
    uint8_t in[H2_BUNDLE_IO_BUF_SIZE];
    uint8_t out[H2_BUNDLE_IO_BUF_SIZE];
    z_stream stream;
    tar_state_t tar;
    int rc;
    int zrc;

    if (installer == NULL || installer->fs == NULL || options == NULL || options->archive_path == NULL || options->dst_root == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }

    memset(&installer->stats, 0, sizeof(installer->stats));
    memset(&tar, 0, sizeof(tar));
    tar.installer = installer;
    tar.options = options;
    tar.kind = TAR_STATE_HEADER;
    rc = init_manifest_seen(&tar);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }

    rc = map_fs(bundle_fs_open(installer->fs, options->archive_path, H2_PAL_FS_OPEN_READ, &archive));
    if (rc != H2_BUNDLE_OK) {
        release_manifest_seen(&tar);
        return rc;
    }

    memset(&stream, 0, sizeof(stream));
    stream.zalloc = zlib_alloc_adapter;
    stream.zfree = zlib_free_adapter;
    stream.opaque = (voidpf)installer->allocator;
    zrc = inflateInit(&stream);
    if (zrc != Z_OK) {
        bundle_fs_close(installer->fs, archive);
        release_manifest_seen(&tar);
        return H2_BUNDLE_ERR_ZLIB;
    }

    do {
        size_t read_len = 0u;
        rc = map_fs(bundle_fs_read(installer->fs, archive, in, sizeof(in), &read_len));
        if (rc != H2_BUNDLE_OK) {
            inflateEnd(&stream);
            bundle_fs_close(installer->fs, archive);
            release_entry_buffers(&tar);
            release_manifest_seen(&tar);
            return rc;
        }
        if (read_len == 0u) {
            inflateEnd(&stream);
            bundle_fs_close(installer->fs, archive);
            release_entry_buffers(&tar);
            release_manifest_seen(&tar);
            return H2_BUNDLE_ERR_ZLIB;
        }

        stream.next_in = in;
        stream.avail_in = (uInt)read_len;
        do {
            stream.next_out = out;
            stream.avail_out = (uInt)sizeof(out);
            zrc = inflate(&stream, Z_NO_FLUSH);
            if (zrc != Z_OK && zrc != Z_STREAM_END) {
                inflateEnd(&stream);
                bundle_fs_close(installer->fs, archive);
                release_entry_buffers(&tar);
                release_manifest_seen(&tar);
                return H2_BUNDLE_ERR_ZLIB;
            }

            size_t have = sizeof(out) - stream.avail_out;
            rc = tar_state_feed(&tar, out, have);
            if (rc != H2_BUNDLE_OK) {
                inflateEnd(&stream);
                bundle_fs_close(installer->fs, archive);
                release_manifest_seen(&tar);
                return rc;
            }
        } while (stream.avail_in > 0u && zrc != Z_STREAM_END);
    } while (zrc != Z_STREAM_END);

    inflateEnd(&stream);
    rc = map_fs(bundle_fs_close(installer->fs, archive));
    if (rc != H2_BUNDLE_OK) {
        release_entry_buffers(&tar);
        release_manifest_seen(&tar);
        return rc;
    }
    rc = tar_state_finish(&tar);
    release_manifest_seen(&tar);
    return rc;
}
