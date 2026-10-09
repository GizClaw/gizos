#include "h2_esp_wakenet.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct model_iface_data { int live, inferred; };
struct h2_pal_fs_file { int live; };
static struct model_iface_data model;
static struct h2_pal_fs_file file;
static srmodel_list_t registry;
static int registry_live, fs_close_fail, model_fail, unsupported_rate;
static int detect_count, create_count, destroy_count, sdk_loads, allocations, alloc_fail_at;
static int misalign_blob;
static void *shifted_storage, *shifted_view;
static uint8_t fixture[128];
static size_t fixture_size, file_offset;

static void u32(uint8_t *ptr, uint32_t n) {
    for (unsigned i = 0u; i < 4u; ++i)
        ptr[i] = (uint8_t)(n >> (i * 8u));
}
static void fixture_init(void) {
    memset(fixture, 0, sizeof(fixture));
    u32(fixture, 1u);
    memcpy(fixture + 4u, "wn9_fixture", 11u);
    u32(fixture + 36u, 1u);
    memcpy(fixture + 40u, "_MODEL_INFO_", 12u);
    u32(fixture + 72u, 80u);
    u32(fixture + 76u, 4u);
    memcpy(fixture + 80u, "info", 4u);
    fixture_size = 84u;
}
static void *allocate(void *user, size_t bytes) {
    (void)user;
    ++allocations;
    if (alloc_fail_at > 0 && allocations == alloc_fail_at)
        return NULL;
    if (misalign_blob && allocations == 2) {
        shifted_storage = malloc(bytes + 31u);
        assert(shifted_storage != NULL);
        shifted_view = (void *)((((uintptr_t)shifted_storage + 15u) &
                                  ~(uintptr_t)15u) + 8u);
        assert(((uintptr_t)shifted_view & 15u) == 8u);
        return shifted_view;
    }
    return malloc(bytes);
}
static void release(void *user, void *ptr) {
    (void)user;
    if (ptr != NULL && ptr == shifted_view) {
        free(shifted_storage);
        shifted_storage = shifted_view = NULL;
    } else {
        free(ptr);
    }
}
static const h2_pal_mem_vtable_t memory_vtable = {.alloc = allocate, .free = release};
static const h2_pal_mem_api_t memory = {.vtable = &memory_vtable};
static int stat_file(void *user, const char *path, h2_pal_fs_stat_t *out) {
    (void)user;
    assert(strcmp(path, "/data/models/wake.bin") == 0);
    *out = (h2_pal_fs_stat_t){.size = fixture_size};
    return H2_PAL_OK;
}
static int open_file(void *user, const char *path, h2_pal_fs_open_mode_t mode,
                       h2_pal_fs_file_t **out) {
    (void)user; (void)path;
    assert(mode == H2_PAL_FS_OPEN_READ && !file.live);
    file.live = 1;
    file_offset = 0u;
    *out = &file;
    return H2_PAL_OK;
}
static int read_file(void *user, h2_pal_fs_file_t *input, void *out,
                       size_t bytes, size_t *read) {
    (void)user;
    assert(input == &file && file.live);
    if (bytes > 7u)
        bytes = 7u;
    if (bytes > fixture_size - file_offset)
        bytes = fixture_size - file_offset;
    memcpy(out, fixture + file_offset, bytes);
    file_offset += bytes;
    *read = bytes;
    return H2_PAL_OK;
}
static int close_file(void *user, h2_pal_fs_file_t *input) {
    (void)user;
    assert(input == &file && file.live);
    if (fs_close_fail) { --fs_close_fail; return H2_PAL_ERR_IO; }
    file.live = 0;
    return H2_PAL_OK;
}
static const h2_pal_fs_vtable_t fs_vtable = {
    .stat = stat_file, .open = open_file, .read = read_file, .close = close_file,
};
static const h2_pal_fs_api_t fs = {.vtable = &fs_vtable};
srmodel_list_t *get_static_srmodels(void) { return registry_live ? &registry : NULL; }
srmodel_list_t *srmodel_load(const void *root) {
    assert(root != NULL && !registry_live);
    assert(((uintptr_t)root & 15u) == 0u);
    assert((((uintptr_t)root + 80u) & 15u) == 0u);
    ++sdk_loads;
    registry_live = 1;
    return &registry;
}
int esp_srmodel_exists(srmodel_list_t *models, char *name) {
    assert(models == &registry);
    return strcmp(name, "wn9_fixture") == 0 ? 0 : -1;
}
void srmodel_host_deinit(srmodel_list_t *models) {
    assert(models == &registry && registry_live);
    registry_live = 0;
}
static model_iface_data_t *create_model(const void *name, det_mode_t mode) {
    assert(strcmp(name, "wn9_fixture") == 0 && mode == DET_MODE_90);
    if (model_fail)
        return NULL;
    ++create_count;
    model.live = 1;
    model.inferred = 0;
    return &model;
}
static int chunks(model_iface_data_t *m) { assert(m == &model); return 4; }
static int rate(model_iface_data_t *m) { assert(m == &model); return unsupported_rate ? 48000 : 16000; }
static int channels(model_iface_data_t *m) { assert(m == &model); return 1; }
static wakenet_state_t detect(model_iface_data_t *m, int16_t *samples) {
    assert(m == &model && model.live);
    model.inferred = 1;
    ++detect_count;
    if (samples[0] == 7) return WAKENET_DETECTED;
    if (samples[0] == 3) return WAKENET_CHANNEL_VERIFIED;
    return WAKENET_NO_DETECT;
}
static void clean(model_iface_data_t *m) {
    (void)m;
    assert(0 && "the SDK clean path is unsafe and must never be called");
}
static void destroy_model(model_iface_data_t *m) {
    assert(m == &model && model.live);
    model.live = 0;
    ++destroy_count;
}
static const esp_wn_iface_t iface = {
    .create = create_model, .get_samp_chunksize = chunks, .get_samp_rate = rate,
    .get_channel_num = channels, .detect = detect, .clean = clean, .destroy = destroy_model,
};
const esp_wn_iface_t *esp_wn_handle_from_name(const char *name) {
    return strcmp(name, "wn9_fixture") == 0 ? &iface : NULL;
}
static h2_esp_wakenet_t *make_detector(void) {
    const h2_esp_wakenet_config_t config = {
        .allocator = &memory, .fs = &fs,
        .model_path = "/data/models/wake.bin", .model_name = "wn9_fixture",
    };
    h2_esp_wakenet_t *detector = NULL;
    assert(h2_esp_wakenet_create(&config, &detector) == H2_PAL_OK);
    return detector;
}
static void test_streaming_and_registry(void) {
    fixture_init();
    h2_esp_wakenet_t *d = make_detector();
    int result = 99;
    int16_t samples[] = {7, 0, 0, 0, 3, 0, 0, 0};
    assert(h2_esp_wakenet_process(d, samples, 1u, &result) == H2_PAL_ERR_INVALID_STATE && result == 0);
    assert(h2_esp_wakenet_open(d) == H2_PAL_OK);
    assert(h2_esp_wakenet_open(d) == H2_PAL_OK);
    h2_esp_wakenet_t *second = make_detector();
    assert(h2_esp_wakenet_open(second) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_esp_wakenet_destroy(&second) == H2_PAL_OK && registry_live);
    assert(h2_esp_wakenet_process(d, samples, 1u, &result) == H2_PAL_OK && result == 0);
    assert(h2_esp_wakenet_process(d, samples + 1u, 3u, &result) == H2_PAL_OK && result == 1);
    assert(h2_esp_wakenet_process(d, samples + 4u, 4u, &result) == H2_PAL_OK && result == 0);
    assert(h2_esp_wakenet_process(d, samples, 1u, &result) == H2_PAL_OK);
    int creates = create_count;
    assert(h2_esp_wakenet_reset(d) == H2_PAL_OK && create_count == creates + 1);
    assert(h2_esp_wakenet_process(d, samples + 1u, 4u, &result) == H2_PAL_OK && result == 0);
    assert(h2_esp_wakenet_destroy(&d) == H2_PAL_OK && d == NULL && !registry_live);
    assert(h2_esp_wakenet_destroy(&d) == H2_PAL_OK);
}
static void test_failures_and_close_retry(void) {
    fixture_init();
    h2_esp_wakenet_t *d = make_detector();
    fs_close_fail = 2;
    assert(h2_esp_wakenet_open(d) == H2_PAL_ERR_IO && file.live);
    assert(h2_esp_wakenet_destroy(&d) == H2_PAL_ERR_IO && d != NULL && file.live);
    assert(h2_esp_wakenet_destroy(&d) == H2_PAL_OK && !file.live);
    model_fail = 1;
    d = make_detector();
    assert(h2_esp_wakenet_open(d) == H2_PAL_ERR_NO_MEMORY && registry_live);
    assert(h2_esp_wakenet_destroy(&d) == H2_PAL_OK && !registry_live);
    model_fail = 0;
    unsupported_rate = 1;
    d = make_detector();
    assert(h2_esp_wakenet_open(d) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_esp_wakenet_destroy(&d) == H2_PAL_OK && !model.live);
    unsupported_rate = 0;
    for (int point = 1; point <= 3; ++point) {
        alloc_fail_at = point;
        allocations = 0;
        const h2_esp_wakenet_config_t cfg = {&memory, &fs, "/data/models/wake.bin", "wn9_fixture"};
        d = NULL;
        int rc = h2_esp_wakenet_create(&cfg, &d);
        if (rc == H2_PAL_OK)
            rc = h2_esp_wakenet_open(d);
        assert(rc == H2_PAL_ERR_NO_MEMORY);
        assert(h2_esp_wakenet_destroy(&d) == H2_PAL_OK);
    }
    alloc_fail_at = 0;
}
static void test_malformed_index_never_reaches_sdk(void) {
    for (int variant = 0; variant < 7; ++variant) {
        fixture_init();
        if (variant == 0) u32(fixture, 0xffffffffu);
        if (variant == 1) u32(fixture + 36u, 99u);
        if (variant == 2) u32(fixture + 72u, 4u);
        if (variant == 3) u32(fixture + 76u, 0xffffffffu);
        if (variant == 4) memset(fixture + 40u, 'x', 32u);
        if (variant == 5) u32(fixture + 72u, 81u);
        if (variant == 6) { u32(fixture + 72u, 84u); fixture_size = 88u; }
        h2_esp_wakenet_t *d = make_detector();
        int loads = sdk_loads;
        assert(h2_esp_wakenet_open(d) == H2_PAL_ERR_FORMAT);
        assert(sdk_loads == loads);
        assert(h2_esp_wakenet_destroy(&d) == H2_PAL_OK);
    }
}
static void test_misaligned_allocator(void) {
    fixture_init();
    allocations = 0;
    misalign_blob = 1;
    h2_esp_wakenet_t *d = make_detector();
    assert(h2_esp_wakenet_open(d) == H2_PAL_OK && shifted_view != NULL);
    assert(h2_esp_wakenet_destroy(&d) == H2_PAL_OK && shifted_view == NULL);
    misalign_blob = 0;
}
static void test_reset_before_first_inference(void) {
    fixture_init();
    h2_esp_wakenet_t *d = make_detector();
    assert(h2_esp_wakenet_open(d) == H2_PAL_OK);
    int creates = create_count, detects = detect_count, result = 0;
    const int16_t samples[] = {7, 0, 0, 0};
    assert(h2_esp_wakenet_reset(d) == H2_PAL_OK);
    assert(h2_esp_wakenet_process(d, samples, 1u, &result) == H2_PAL_OK);
    assert(h2_esp_wakenet_reset(d) == H2_PAL_OK && create_count == creates);
    assert(h2_esp_wakenet_process(d, samples + 1u, 3u, &result) == H2_PAL_OK);
    assert(detect_count == detects);
    assert(h2_esp_wakenet_process(d, samples + 3u, 1u, &result) == H2_PAL_OK);
    assert(detect_count == detects + 1 && result == 0);
    assert(h2_esp_wakenet_reset(d) == H2_PAL_OK && create_count == creates + 1);
    assert(h2_esp_wakenet_reset(d) == H2_PAL_OK && create_count == creates + 1);
    assert(h2_esp_wakenet_destroy(&d) == H2_PAL_OK);
}
static void test_reset_recreation_failure(void) {
    fixture_init();
    h2_esp_wakenet_t *d = make_detector();
    assert(h2_esp_wakenet_open(d) == H2_PAL_OK);
    const int16_t samples[] = {0, 0, 0, 0};
    int result = 0;
    assert(h2_esp_wakenet_process(d, samples, 4u, &result) == H2_PAL_OK);
    model_fail = 1;
    assert(h2_esp_wakenet_reset(d) == H2_PAL_ERR_NO_MEMORY && !model.live);
    assert(h2_esp_wakenet_process(d, samples, 4u, &result) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_esp_wakenet_destroy(&d) == H2_PAL_OK && !registry_live);
    model_fail = 0;
}
int main(void) {
    test_streaming_and_registry();
    test_failures_and_close_retry();
    test_malformed_index_never_reaches_sdk();
    test_misaligned_allocator();
    test_reset_before_first_inference();
    test_reset_recreation_failure();
    assert(!registry_live && !file.live && !model.live && detect_count == 5);
    assert(create_count == destroy_count);
    return 0;
}
