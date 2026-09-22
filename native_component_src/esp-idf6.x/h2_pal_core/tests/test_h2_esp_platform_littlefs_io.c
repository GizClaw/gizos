#include "h2_esp_platform_littlefs_io.h"
#include "h2_esp_platform_safe_call.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Like the device worker: run on a copy of the context, then copy it back. */
_Alignas(max_align_t) static uint8_t s_worker_context[4096];
static uint8_t s_scratch[16u * 1024u];

h2_pal_result_t h2_esp_platform_safe_call(
    h2_esp_platform_safe_call_cb_t callback, void *context,
    size_t context_size, size_t stack_depth) {
    (void)stack_depth;
    assert(context_size <= sizeof(s_worker_context));
    memcpy(s_worker_context, context, context_size);
    memset(context, 0xa5, context_size);
    callback(s_worker_context);
    memcpy(context, s_worker_context, context_size);
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_platform_safe_io_acquire(uint8_t **out_buffer,
                                                size_t *out_capacity) {
    *out_buffer = s_scratch;
    *out_capacity = sizeof(s_scratch);
    return H2_PAL_OK;
}

void h2_esp_platform_safe_io_release(void) {}

int main(void) {
    char root[128];
    char path[512];
    uint8_t one = 1u;
    h2_esp_pref_store_t store = {0};
    FILE *file;

    snprintf(root, sizeof(root), "/tmp/h2-pref-io-%ld", (long)getpid());
    assert(mkdir(root, 0700) == 0);
    store.base_path = root;
    store.committed_budget = 128u * 1024u;
    assert(h2_esp_pref_io_prepare(&store) == H2_PAL_OK);
    assert(!store.committed_total_valid);

    /* 20-byte header + "io/ns" (5) + key (3) + 1-byte value = 29 bytes. */
    assert(h2_esp_pref_io_set(&store, "io/ns", "aaa", H2_PAL_PREF_ENTRY_BOOL,
                              &one, sizeof(one)) == H2_PAL_OK);
    assert(store.committed_total_valid && store.committed_total == 29u);

    /* A record the walk cannot decode: a set that rescanned would fail. */
    snprintf(path, sizeof(path), "%s/696f2f6e73/626262", root);
    file = fopen(path, "wb");
    assert(file != NULL);
    assert(fputs("not a record", file) >= 0);
    assert(fclose(file) == 0);
    assert(h2_esp_pref_io_set(&store, "io/ns", "ccc", H2_PAL_PREF_ENTRY_BOOL,
                              &one, sizeof(one)) == H2_PAL_OK);
    assert(store.committed_total_valid && store.committed_total == 58u);

    assert(h2_esp_pref_io_remove(&store, "io/ns", "aaa") == H2_PAL_OK);
    assert(store.committed_total_valid && store.committed_total == 29u);

    /* Prepare invalidates, so the next set walks and meets the bad record. */
    assert(h2_esp_pref_io_prepare(&store) == H2_PAL_OK);
    assert(!store.committed_total_valid);
    assert(h2_esp_pref_io_set(&store, "io/ns", "ddd", H2_PAL_PREF_ENTRY_BOOL,
                              &one, sizeof(one)) == H2_PAL_ERR_IO);

    assert(unlink(path) == 0);
    assert(h2_esp_pref_io_clear(&store, "io/ns") == H2_PAL_OK);
    assert(!store.committed_total_valid);
    store.committed_budget = 29u;
    assert(h2_esp_pref_io_set(&store, "io/ns", "eee", H2_PAL_PREF_ENTRY_BOOL,
                              &one, sizeof(one)) == H2_PAL_OK);
    assert(store.committed_total_valid && store.committed_total == 29u);
    assert(h2_esp_pref_io_set(&store, "io/ns", "fff", H2_PAL_PREF_ENTRY_BOOL,
                              &one, sizeof(one)) == H2_PAL_ERR_NO_SPACE);
    assert(h2_esp_pref_io_clear(&store, "io/ns") == H2_PAL_OK);
    assert(!store.committed_total_valid);
    assert(rmdir(root) == 0);
    return 0;
}
