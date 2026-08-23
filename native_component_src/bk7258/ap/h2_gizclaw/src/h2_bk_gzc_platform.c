#include "gzc_platform.h"

#include "os/mem.h"
#include "os/os.h"

#include <driver/trng.h>
#include <time.h>

static void *platform_malloc(void *user, size_t size) {
    (void)user;
    return os_malloc(size);
}

static void *platform_realloc(void *user, void *ptr, size_t size) {
    (void)user;
    return os_realloc(ptr, size);
}

static void platform_free(void *user, void *ptr) {
    (void)user;
    os_free(ptr);
}

static int64_t platform_time_instant_ms(void *user) {
    (void)user;
    return (int64_t)rtos_get_time();
}

static int64_t platform_time_unix_ms(void *user) {
    (void)user;
    return (int64_t)time(NULL) * 1000;
}

static int platform_random(void *user, uint8_t *out, size_t len) {
    (void)user;
    if (out == NULL && len != 0u) {
        return GZC_ERR_INVALID_ARGUMENT;
    }
    if (len == 0u) {
        return GZC_OK;
    }
    return bk_fill_rand(out, len) == BK_OK ? GZC_OK : GZC_ERR_RPC;
}

static void platform_log(
    void *user,
    gzc_log_level_t level,
    gzc_str_t message) {
    (void)user;
    (void)level;
    (void)message;
}

const gzc_platform_t *gzc_default_platform(void) {
    static const gzc_platform_t platform = {
        .malloc = platform_malloc,
        .realloc = platform_realloc,
        .free = platform_free,
        .time_instant_ms = platform_time_instant_ms,
        .time_unix_ms = platform_time_unix_ms,
        .random = platform_random,
        .log = platform_log,
    };
    return &platform;
}
