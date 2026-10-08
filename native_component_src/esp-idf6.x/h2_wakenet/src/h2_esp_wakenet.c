#include "h2_esp_wakenet.h"

#include "esp_wn_models.h"
#include "model_path.h"

#include <stdbool.h>
#include <string.h>

#define MODEL_MAX_BYTES (1024u * 1024u)
#define MODEL_ALIGNMENT 16u

struct h2_esp_wakenet {
    h2_esp_wakenet_config_t config;
    h2_pal_fs_file_t *file;
    uint8_t *blob_storage;
    uint8_t *blob;
    size_t blob_size;
    srmodel_list_t *models;
    const esp_wn_iface_t *iface;
    model_iface_data_t *model;
    int16_t *chunk;
    size_t chunk_samples;
    size_t filled;
    bool opened;
};

static uint32_t read_u32(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8u |
        (uint32_t)data[2] << 16u | (uint32_t)data[3] << 24u;
}

/* Validate the SDK's pointer-bearing index before its unsized srmodel_load(). */
static bool valid_blob(const uint8_t *blob, size_t size) {
    if (size < 4u)
        return false;
    uint32_t models = read_u32(blob);
    if (models == 0u || models > 16u)
        return false;
    size_t cursor = 4u;
    for (uint32_t i = 0u; i < models; ++i) {
        if (cursor > size || size - cursor < 36u ||
            memchr(blob + cursor, 0, 32u) == NULL || blob[cursor] == 0u)
            return false;
        uint32_t files = read_u32(blob + cursor + 32u);
        if (files == 0u || files > 32u)
            return false;
        cursor += 36u;
        if (cursor > size || (size_t)files > (size - cursor) / 40u)
            return false;
        for (uint32_t j = 0u; j < files; ++j) {
            if (blob[cursor] == 0u || memchr(blob + cursor, 0, 32u) == NULL)
                return false;
            cursor += 40u;
        }
    }
    const size_t header_end = cursor;
    cursor = 4u;
    for (uint32_t i = 0u; i < models; ++i) {
        uint32_t files = read_u32(blob + cursor + 32u);
        cursor += 36u;
        bool info = false;
        for (uint32_t j = 0u; j < files; ++j) {
            uint32_t offset = read_u32(blob + cursor + 32u);
            uint32_t length = read_u32(blob + cursor + 36u);
            if (offset < header_end || offset > size || length == 0u ||
                length > size - offset || (offset & (MODEL_ALIGNMENT - 1u)) != 0u)
                return false;
            if (strcmp((const char *)blob + cursor, "_MODEL_INFO_") == 0)
                info = true;
            cursor += 40u;
        }
        if (!info)
            return false;
    }
    return true;
}

h2_pal_result_t h2_esp_wakenet_create(const h2_esp_wakenet_config_t *config,
                                     h2_esp_wakenet_t **out_detector) {
    if (out_detector == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out_detector = NULL;
    if (config == NULL || config->allocator == NULL ||
        config->allocator->vtable == NULL || config->allocator->vtable->alloc == NULL ||
        config->allocator->vtable->free == NULL || config->fs == NULL ||
        config->fs->vtable == NULL || config->fs->vtable->stat == NULL ||
        config->fs->vtable->open == NULL || config->fs->vtable->read == NULL ||
        config->fs->vtable->close == NULL ||
        config->model_path == NULL || config->model_path[0] == '\0' ||
        config->model_name == NULL || config->model_name[0] == '\0' ||
        strlen(config->model_name) >= 32u)
        return H2_PAL_ERR_INVALID_ARG;
    h2_esp_wakenet_t *detector = h2_pal_mem_alloc(config->allocator, sizeof(*detector));
    if (detector == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    memset(detector, 0, sizeof(*detector));
    detector->config = *config;
    *out_detector = detector;
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_wakenet_open(h2_esp_wakenet_t *d) {
    if (d == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    if (d->opened)
        return H2_PAL_OK;
    if (d->file != NULL || d->blob != NULL || get_static_srmodels() != NULL)
        return H2_PAL_ERR_INVALID_STATE;
    h2_pal_fs_stat_t stat = {0};
    h2_pal_result_t rc = h2_pal_fs_stat(d->config.fs, d->config.model_path, &stat);
    if (rc != H2_PAL_OK)
        return rc;
    if (stat.is_dir || stat.size < 4u || stat.size > MODEL_MAX_BYTES)
        return H2_PAL_ERR_FORMAT;
    d->blob_size = (size_t)stat.size;
    d->blob_storage = h2_pal_mem_alloc(
        d->config.allocator, d->blob_size + MODEL_ALIGNMENT - 1u);
    if (d->blob_storage == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    uintptr_t base = (uintptr_t)d->blob_storage;
    d->blob = (uint8_t *)((base + MODEL_ALIGNMENT - 1u) &
                         ~(uintptr_t)(MODEL_ALIGNMENT - 1u));
    rc = h2_pal_fs_open(d->config.fs, d->config.model_path, H2_PAL_FS_OPEN_READ, &d->file);
    if (rc != H2_PAL_OK)
        return rc;
    size_t offset = 0u;
    while (offset < d->blob_size) {
        size_t read = 0u;
        rc = h2_pal_fs_read(d->config.fs, d->file, d->blob + offset,
                            d->blob_size - offset, &read);
        if (rc != H2_PAL_OK)
            return rc;
        if (read == 0u || read > d->blob_size - offset)
            return H2_PAL_ERR_FORMAT;
        offset += read;
    }
    rc = h2_pal_fs_close(d->config.fs, d->file);
    if (rc != H2_PAL_OK)
        return rc;
    d->file = NULL;
    if (!valid_blob(d->blob, d->blob_size))
        return H2_PAL_ERR_FORMAT;
    d->models = srmodel_load(d->blob);
    if (d->models == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    if (esp_srmodel_exists(d->models, (char *)d->config.model_name) < 0)
        return H2_PAL_ERR_NOT_FOUND;
    d->iface = esp_wn_handle_from_name(d->config.model_name);
    if (d->iface == NULL || d->iface->create == NULL || d->iface->detect == NULL ||
        d->iface->clean == NULL || d->iface->destroy == NULL ||
        d->iface->get_samp_chunksize == NULL || d->iface->get_samp_rate == NULL ||
        d->iface->get_channel_num == NULL)
        return H2_PAL_ERR_UNSUPPORTED;
    d->model = d->iface->create(d->config.model_name, DET_MODE_90);
    if (d->model == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    int count = d->iface->get_samp_chunksize(d->model);
    if (count <= 0 || count > 4096 || d->iface->get_samp_rate(d->model) != 16000 ||
        d->iface->get_channel_num(d->model) != 1)
        return H2_PAL_ERR_UNSUPPORTED;
    d->chunk_samples = (size_t)count;
    d->chunk = h2_pal_mem_alloc(d->config.allocator, d->chunk_samples * sizeof(int16_t));
    if (d->chunk == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    d->opened = true;
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_wakenet_process(h2_esp_wakenet_t *d, const int16_t *pcm,
                                      size_t samples, int *out_detected) {
    if (out_detected == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out_detected = 0;
    if (d == NULL || pcm == NULL || samples == 0u)
        return H2_PAL_ERR_INVALID_ARG;
    if (!d->opened)
        return H2_PAL_ERR_INVALID_STATE;
    while (samples > 0u) {
        size_t count = d->chunk_samples - d->filled;
        if (count > samples)
            count = samples;
        memcpy(d->chunk + d->filled, pcm, count * sizeof(*pcm));
        d->filled += count;
        pcm += count;
        samples -= count;
        if (d->filled == d->chunk_samples) {
            wakenet_state_t result = d->iface->detect(d->model, d->chunk);
            if (result == WAKENET_DETECTED)
                *out_detected = 1;
            d->filled = 0u;
        }
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_wakenet_reset(h2_esp_wakenet_t *d) {
    if (d == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    if (!d->opened)
        return H2_PAL_ERR_INVALID_STATE;
    d->iface->clean(d->model);
    d->filled = 0u;
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_wakenet_close(h2_esp_wakenet_t *d) {
    if (d == NULL)
        return H2_PAL_OK;
    d->opened = false;
    if (d->file != NULL) {
        h2_pal_result_t rc = h2_pal_fs_close(d->config.fs, d->file);
        if (rc != H2_PAL_OK)
            return rc;
        d->file = NULL;
    }
    if (d->model != NULL)
        d->iface->destroy(d->model);
    d->model = NULL;
    if (d->models != NULL)
        srmodel_host_deinit(d->models);
    d->models = NULL;
    h2_pal_mem_free(d->config.allocator, d->chunk);
    h2_pal_mem_free(d->config.allocator, d->blob_storage);
    d->chunk = NULL;
    d->blob = NULL;
    d->blob_storage = NULL;
    d->filled = 0u;
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_wakenet_destroy(h2_esp_wakenet_t **detector) {
    if (detector == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    if (*detector == NULL)
        return H2_PAL_OK;
    h2_pal_result_t rc = h2_esp_wakenet_close(*detector);
    if (rc != H2_PAL_OK)
        return rc;
    h2_pal_mem_free((*detector)->config.allocator, *detector);
    *detector = NULL;
    return H2_PAL_OK;
}
