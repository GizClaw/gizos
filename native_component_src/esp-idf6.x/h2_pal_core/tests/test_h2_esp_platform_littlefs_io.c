#include "h2_esp_platform_littlefs_io.h"
#include "h2_esp_platform_safe_call.h"
#include "h2_esp_io_phase.h"

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Like the device worker: run on a copy of the context, then copy it back. */
_Alignas(max_align_t) static uint8_t s_worker_context[4096];
static uint8_t s_scratch[16u * 1024u];
#if H2_ESP_IO_PHASE_DIAGNOSTICS
static int s_scratch_held;
static unsigned s_get_records, s_set_records;
static int64_t s_now;
int64_t esp_timer_get_time(void) { return s_now; }
int h2_io_phase_test_printf(const char *format, ...) {
    assert(!s_scratch_held);
    char record[1024];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(record, sizeof(record), format, args);
    va_end(args);
    assert(n > 0 && (size_t)n < sizeof(record));
    assert(strstr(record, "namespace=") == NULL &&
           strstr(record, "key=") == NULL && strstr(record, "io/ns") == NULL);
    if (strstr(record, "owner=pref op=2 ")) {
        ++s_get_records;
        assert(strstr(record, "scratch_wait_us=7000 ") != NULL);
    }
    if (strstr(record, "owner=pref op=3 ")) {
        ++s_set_records;
        assert(strstr(record, "scratch_wait_us=7000 ") != NULL);
    }
    return n;
}
h2_pal_result_t h2_esp_platform_safe_call_timed(
    h2_esp_platform_safe_call_cb_t callback, void *context,
    size_t context_size, size_t stack_depth, h2_esp_io_phase_t *phase) {
    const uint64_t started = h2_esp_io_phase_now();
    const h2_pal_result_t rc = h2_esp_platform_safe_call(
        callback, context, context_size, stack_depth);
    s_now += 150000;
    phase->native_us = h2_esp_io_phase_elapsed(started, h2_esp_io_phase_now());
    phase->native_max_us = phase->native_us;
    phase->calls = phase->direct_calls = 1u;
    return rc;
}
#endif

h2_pal_result_t h2_esp_platform_safe_call(
    h2_esp_platform_safe_call_cb_t callback, void *context,
    size_t context_size, size_t stack_depth) {
    (void)stack_depth;
    assert(context_size <= 1024u);
    memcpy(s_worker_context, context, context_size);
    memset(context, 0xa5, context_size);
    callback(s_worker_context);
    memcpy(context, s_worker_context, context_size);
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_platform_safe_io_acquire(uint8_t **out_buffer,
                                                size_t *out_capacity) {
#if H2_ESP_IO_PHASE_DIAGNOSTICS
    assert(!s_scratch_held); s_scratch_held = 1; s_now += 7000;
#endif
    *out_buffer = s_scratch;
    *out_capacity = sizeof(s_scratch);
    return H2_PAL_OK;
}

void h2_esp_platform_safe_io_release(void) {
#if H2_ESP_IO_PHASE_DIAGNOSTICS
    assert(s_scratch_held); s_scratch_held = 0;
#endif
}

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

#if H2_ESP_IO_PHASE_DIAGNOSTICS
    uint8_t *read = NULL;
    size_t size = 0u;
    assert(h2_esp_pref_io_get(&store, "io/ns", "aaa", H2_PAL_PREF_ENTRY_BOOL,
                            &read, &size) == H2_PAL_OK);
    assert(size == 1u && read[0] == one);
    free(read);
    assert(s_get_records == 1u && s_set_records == 1u && !s_scratch_held);
#endif

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
#if H2_ESP_IO_PHASE_DIAGNOSTICS
    assert(s_set_records == 5u && !s_scratch_held);
#endif
    return 0;
}
