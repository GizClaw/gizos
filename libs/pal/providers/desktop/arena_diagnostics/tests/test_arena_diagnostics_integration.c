#include "h2_mem_arena_diagnostics.h"
#include "h2_desktop_platform.h"

#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct memory_fs memory_fs_t;
struct h2_pal_fs_file { memory_fs_t *owner; };
struct memory_fs {
    char *text;
    size_t used, capacity, syncs;
    bool closed;
    struct h2_pal_fs_file file;
    h2_pal_fs_api_t api;
};

static int fs_open(void *user, const char *path, h2_pal_fs_open_mode_t mode,
                   h2_pal_fs_file_t **out_file) {
    memory_fs_t *fs = user;
    assert(strcmp(path, "/report/trace.log") == 0);
    assert(mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE);
    fs->used = 0u;
    fs->closed = false;
    *out_file = &fs->file;
    return H2_PAL_OK;
}
static int fs_write(void *user, h2_pal_fs_file_t *file, const void *data,
                    size_t len, size_t *out_written) {
    memory_fs_t *fs = user;
    assert(file == &fs->file && !fs->closed);
    const size_t count = len < 17u ? len : 17u;
    assert(count <= fs->capacity - fs->used - 1u);
    memcpy(fs->text + fs->used, data, count);
    fs->used += count;
    fs->text[fs->used] = '\0';
    *out_written = count;
    return H2_PAL_OK;
}
static int fs_sync(void *user, h2_pal_fs_file_t *file) {
    memory_fs_t *fs = user;
    assert(file == &fs->file && !fs->closed);
    ++fs->syncs;
    return H2_PAL_OK;
}
static int fs_close(void *user, h2_pal_fs_file_t *file) {
    memory_fs_t *fs = user;
    assert(file == &fs->file && !fs->closed);
    fs->closed = true;
    return H2_PAL_OK;
}
static const h2_pal_fs_vtable_t k_fs_vtable = {
    .open = fs_open, .write = fs_write, .sync = fs_sync, .close = fs_close};

static h2_pal_result_t safe_copy(void *user, const void *source,
                                  void *destination, size_t bytes) {
    assert(user != NULL);
    memcpy(destination, source, bytes);
    return H2_PAL_OK;
}
static h2_pal_result_t capture_frames(void *user, uintptr_t *frames,
                                       size_t capacity, size_t *out_count) {
    assert(user != NULL && capacity >= 2u);
    frames[0] = 0x100u;
    frames[1] = 0x200u;
    *out_count = 2u;
    return H2_PAL_OK;
}
static const char *const k_tags[] = {"other", "service", "image", "stack"};

static h2_mem_arena_diagnostics_config_t config_for(void *backing,
                                                      memory_fs_t *fs) {
    return (h2_mem_arena_diagnostics_config_t){
        .name = "c11-diag-test", .backing = backing,
        .backing_bytes = 1024u * 1024u,
        .small_request_max = 32768u,
        .small_pool_bytes = 256u * 1024u,
        .fallback = h2_desktop_platform_default_allocator(),
        .metadata = h2_desktop_platform_default_allocator(),
        .sync = h2_desktop_platform_sync_api(),
        .task = h2_desktop_platform_task_api(),
        .queue = h2_desktop_platform_queue_api(),
        .time = h2_desktop_platform_time_api(),
        .report_fs = &fs->api, .report_path = "/report/trace.log",
        .tags = k_tags, .tag_count = 4u, .stack_tag_index = 3u,
        .probe = {.user = fs, .safe_copy = safe_copy,
                  .capture_frames = capture_frames},
        .trace = true, .block_capacity = 2u, .site_capacity = 3u,
        .peak_capacity = 2u, .duplicate_capacity = 2u,
        .report_queue_capacity = 1u, .top_count = 2u,
        .report_task_name = "arena/diagnostics-test",
        .report_task_stack_size = 32768u,
        .case_end_prefix = "case_end:"};
}

typedef struct worker {
    const h2_pal_mem_api_t *mem;
} worker_t;
static void *exercise_allocations(void *user) {
    const worker_t *worker = user;
    for (unsigned i = 0u; i < 100u; ++i) {
        void *ptr = h2_pal_mem_alloc(worker->mem, 64u);
        assert(ptr != NULL);
        memset(ptr, 0x4c, 64u);
        h2_pal_mem_free(worker->mem, ptr);
    }
    return NULL;
}

int main(void) {
    void *backing = malloc(1024u * 1024u);
    assert(backing != NULL);
    memory_fs_t fs = {0};
    fs.capacity = 1024u * 1024u;
    fs.text = malloc(fs.capacity);
    assert(fs.text != NULL);
    fs.file.owner = &fs;
    fs.api = (h2_pal_fs_api_t){.user = &fs, .vtable = &k_fs_vtable};
    h2_mem_arena_diagnostics_config_t cfg = config_for(backing, &fs);
    h2_mem_arena_diagnostics_t *d = (void *)1;
    cfg.backing = NULL;
    assert(h2_mem_arena_diagnostics_create(&cfg, &d) ==
           H2_PAL_ERR_INVALID_ARG && d == NULL);
    cfg.backing = backing;
    assert(h2_mem_arena_diagnostics_create(&cfg, &d) == H2_PAL_OK);
    const h2_pal_mem_api_t *service = h2_mem_arena_diagnostics_mem(d, "service");
    const h2_pal_mem_api_t *image = h2_mem_arena_diagnostics_mem(d, "image");
    assert(service != NULL && image != NULL && service != image);
    assert(h2_mem_arena_diagnostics_mem(d, "unknown") ==
           h2_mem_arena_diagnostics_mem(d, "other"));
    void *a = h2_pal_mem_alloc(service, 1024u);
    void *b = h2_pal_mem_alloc(image, 16384u);
    void *c = h2_pal_mem_alloc(image, 16384u);
    assert(a != NULL && b != NULL && c != NULL);
    memset(a, 0x41, 100u);
    memset(b, 0x42, 16384u);
    memcpy(c, b, 16384u);
    assert(h2_mem_arena_diagnostics_destroy(d) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_mem_arena_diagnostics_report(d, "case_end:first") == H2_PAL_OK);
    assert(h2_mem_arena_diagnostics_flush(d) == H2_PAL_OK);
    assert(strstr(fs.text, "BLOCK ") != NULL);
    assert(strstr(fs.text, "OVERFLOW ") != NULL);
    assert(h2_pal_mem_realloc(image, a, SIZE_MAX) == NULL);
    a = h2_pal_mem_realloc(image, a, 2048u);
    assert(a != NULL);
    for (size_t i = 0u; i < 100u; ++i)
        assert(((unsigned char *)a)[i] == 0x41u);
    h2_pal_mem_free(image, a);
    h2_pal_mem_free(service, b);
    h2_pal_mem_free(service, c);
    worker_t worker = {.mem = service};
    pthread_t threads[4];
    for (size_t i = 0u; i < 4u; ++i)
        assert(pthread_create(&threads[i], NULL, exercise_allocations,
                              &worker) == 0);
    for (size_t i = 0u; i < 4u; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(h2_mem_arena_diagnostics_report(d, "case_end:second") == H2_PAL_OK);
    assert(h2_mem_arena_diagnostics_destroy(d) == H2_PAL_OK);
    assert(fs.closed && fs.syncs >= 2u);
    assert(strstr(fs.text, "END live=0") != NULL);
    assert(strstr(fs.text, "SUMMARY_TAG tag=service") != NULL);
    free(fs.text);
    free(backing);
}
