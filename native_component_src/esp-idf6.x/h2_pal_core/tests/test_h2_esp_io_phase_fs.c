#include "h2_esp_platform_core.h"
#include "h2_esp_io_phase.h"

#include "esp_littlefs.h"
#include "freertos/semphr.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int64_t s_now;
static int s_fs_held, s_scratch_held;
static unsigned s_reads, s_writes;
static uint8_t s_scratch[16384];
int64_t esp_timer_get_time(void) { return s_now; }
int h2_io_phase_test_printf(const char *format, ...) {
    assert(!s_fs_held && !s_scratch_held);
    char record[1024];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(record, sizeof(record), format, args);
    va_end(args);
    assert(n > 0 && (size_t)n < sizeof(record));
    assert(strstr(record, "path=") == NULL);
    if (strstr(record, "owner=fs op=3 ") ||
        strstr(record, "owner=fs op=5 ")) {
        assert(strstr(record, "bytes=9000 rc=0 ") != NULL);
        assert(strstr(record, "fs_wait_us=15000 ") != NULL);
        assert(strstr(record, "scratch_wait_us=7000 ") != NULL);
        assert(strstr(record, "native_us=330000 ") != NULL);
        assert(strstr(record, "native_max_us=110000 ") != NULL);
        assert(strstr(record, "calls=3 ") != NULL);
        if (strstr(record, "op=3 ")) ++s_reads;
        else ++s_writes;
    }
    return n;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { return s; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, uint32_t timeout) {
    assert(s != NULL && timeout == portMAX_DELAY && !s_fs_held);
    s_fs_held = 1; s_now += 5000; return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    assert(s != NULL && s_fs_held); s_fs_held = 0; return pdTRUE;
}
h2_pal_result_t h2_esp_platform_safe_io_acquire(
    uint8_t **data, size_t *capacity) {
    assert(!s_scratch_held); s_scratch_held = 1; s_now += 7000;
    *data = s_scratch; *capacity = sizeof(s_scratch); return H2_PAL_OK;
}
void h2_esp_platform_safe_io_release(void) {
    assert(s_scratch_held); s_scratch_held = 0;
}
h2_pal_result_t h2_esp_platform_safe_call_timed(
    h2_esp_platform_safe_call_cb_t callback, void *context,
    size_t context_size, size_t stack_depth, h2_esp_io_phase_t *phase) {
    assert(context_size <= 1024u && stack_depth == 4096u && s_fs_held);
    _Alignas(max_align_t) uint8_t copy[1024];
    memcpy(copy, context, context_size);
    callback(copy);
    memcpy(context, copy, context_size);
    s_now += 110000;
    phase->native_us = phase->native_max_us = 110000;
    phase->calls = phase->direct_calls = 1u;
    return H2_PAL_OK;
}
bool esp_littlefs_mounted(const char *label) { (void)label; return false; }
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *config) {
    assert(config != NULL); return ESP_OK;
}
esp_err_t esp_vfs_littlefs_unregister(const char *label) {
    (void)label; return ESP_OK;
}
esp_err_t esp_littlefs_format(const char *label) {
    (void)label; assert(!"format is outside this test"); return ESP_OK;
}

int main(void) {
    char root[128];
    snprintf(root, sizeof(root), "/tmp/h2-fs-phase-%ld", (long)getpid());
    assert(mkdir(root, 0700) == 0);
    h2_pal_fs_api_t fs;
    assert(h2_esp_platform_littlefs_fs_use_base_path(&fs, root) == H2_PAL_OK);
    uint8_t data[9000], read[9000];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)i;
    h2_pal_fs_file_t *file = NULL;
    assert(h2_pal_fs_open(&fs, "/sample", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file) == H2_PAL_OK);
    size_t written = 0;
    assert(h2_pal_fs_write(&fs, file, data, sizeof(data), &written) == H2_PAL_OK);
    assert(written == sizeof(data));
    assert(h2_pal_fs_close(&fs, file) == H2_PAL_OK);
    assert(h2_pal_fs_open(&fs, "/sample", H2_PAL_FS_OPEN_READ, &file) == H2_PAL_OK);
    size_t received = 0;
    assert(h2_pal_fs_read(&fs, file, read, sizeof(read), &received) == H2_PAL_OK);
    assert(received == sizeof(read) && !memcmp(data, read, sizeof(data)));
    assert(h2_pal_fs_close(&fs, file) == H2_PAL_OK);
    assert(s_reads == 1u && s_writes == 1u && !s_fs_held && !s_scratch_held);
    assert(h2_pal_fs_remove(&fs, "/sample") == H2_PAL_OK);
    assert(rmdir(root) == 0);
    return 0;
}
