#include "h2_windows_platform.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
    const h2_windows_platform_config_t config = {0};
    h2_windows_platform_t *platform = NULL;
    assert(h2_windows_platform_create(&config, &platform) == H2_PAL_OK);

    const h2_pal_mem_api_t *mem = h2_windows_mem_api(platform);
    void *allocation = h2_pal_mem_alloc(mem, 32u);
    assert(allocation != NULL);
    assert(h2_windows_platform_destroy(&platform) ==
           H2_PAL_ERR_INVALID_STATE);
    h2_pal_mem_free(mem, allocation);

    h2_pal_queue_config_t queue_config = {
        .name = "windows-os-test",
        .item_size = sizeof(uint32_t),
        .item_count = 1u,
        .allocator = mem,
    };
    h2_pal_queue_t *queue = NULL;
    assert(h2_pal_queue_create(h2_windows_queue_api(platform), &queue_config,
                               &queue) == H2_PAL_OK);
    uint32_t value = 0u;
    assert(h2_pal_queue_recv(h2_windows_queue_api(platform), queue, &value,
                             H2_PAL_QUEUE_NO_WAIT) == H2_PAL_ERR_TIMEOUT);
    value = 1u;
    assert(h2_pal_queue_send(h2_windows_queue_api(platform), queue, &value,
                             H2_PAL_QUEUE_NO_WAIT) == H2_PAL_OK);
    value = 2u;
    assert(h2_pal_queue_send(h2_windows_queue_api(platform), queue, &value,
                             H2_PAL_QUEUE_NO_WAIT) == H2_PAL_ERR_TIMEOUT);
    assert(h2_pal_queue_send_latest(h2_windows_queue_api(platform), queue,
                                    &value) == H2_PAL_OK);
    value = 0u;
    assert(h2_pal_queue_recv(h2_windows_queue_api(platform), queue, &value,
                             H2_PAL_QUEUE_NO_WAIT) == H2_PAL_OK);
    assert(value == 2u);
    assert(h2_pal_queue_reset(h2_windows_queue_api(platform), queue) ==
           H2_PAL_OK);
    assert(h2_pal_queue_close(h2_windows_queue_api(platform), queue) ==
           H2_PAL_OK);
    assert(h2_pal_queue_recv(h2_windows_queue_api(platform), queue, &value,
                             H2_PAL_QUEUE_NO_WAIT) == H2_PAL_ERR_CLOSED);
    h2_pal_queue_destroy(h2_windows_queue_api(platform), queue);

    const h2_pal_sync_api_t *sync = h2_windows_sync_api(platform);
    h2_pal_semaphore_config_t semaphore_config = {
        .name = "windows-os-test",
        .allocator = mem,
        .initial_count = 0u,
        .max_count = 1u,
    };
    h2_pal_semaphore_t *semaphore = NULL;
    assert(h2_pal_semaphore_create(sync, &semaphore_config, &semaphore) ==
           H2_PAL_OK);
    assert(h2_pal_semaphore_take(sync, semaphore, H2_PAL_SYNC_NO_WAIT) ==
           H2_PAL_ERR_TIMEOUT);
    assert(h2_pal_semaphore_give(sync, semaphore) == H2_PAL_OK);
    assert(h2_pal_semaphore_take(sync, semaphore, H2_PAL_SYNC_NO_WAIT) ==
           H2_PAL_OK);
    assert(h2_pal_semaphore_destroy(sync, semaphore) == H2_PAL_OK);

    h2_pal_mutex_config_t mutex_config = {
        .name = "windows-os-test",
        .allocator = mem,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    h2_pal_cond_config_t cond_config = {
        .name = "windows-os-test",
        .allocator = mem,
    };
    h2_pal_mutex_t *mutex = NULL;
    h2_pal_cond_t *condition = NULL;
    assert(h2_pal_mutex_create(sync, &mutex_config, &mutex) == H2_PAL_OK);
    assert(h2_pal_cond_create(sync, &cond_config, &condition) == H2_PAL_OK);
    assert(h2_pal_mutex_lock(sync, mutex) == H2_PAL_OK);
    assert(h2_pal_cond_wait(sync, condition, mutex, 1u) ==
           H2_PAL_ERR_TIMEOUT);
    assert(h2_pal_mutex_unlock(sync, mutex) == H2_PAL_OK);
    assert(h2_pal_cond_signal(sync, condition) == H2_PAL_OK);
    assert(h2_pal_cond_broadcast(sync, condition) == H2_PAL_OK);
    assert(h2_pal_cond_destroy(sync, condition) == H2_PAL_OK);
    assert(h2_pal_mutex_destroy(sync, mutex) == H2_PAL_OK);

    assert(h2_windows_platform_destroy(&platform) == H2_PAL_OK);
    return 0;
}
