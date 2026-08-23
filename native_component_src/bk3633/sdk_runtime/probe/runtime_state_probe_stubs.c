#include "h2_bk3633_sdk_runtime.h"
#include "h2_bk3633_sdk_runtime_internal.h"
#include "runtime_state_probe_stubs.h"

#include "co_utils.h"
#include "flash.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct bd_addr co_default_bdaddr;
h2_bk3633_probe_flash_env_t flash_env;

static int s_application_init_count;
static int s_schedule_count;
static int s_nvds_result;
static h2_pal_result_t s_rom_environment_result;

static void *probe_alloc(void *user, size_t size)
{
    (void)user;
    return malloc(size);
}

static void *probe_realloc(void *user, void *memory, size_t size)
{
    (void)user;
    return realloc(memory, size);
}

static void probe_free(void *user, void *memory)
{
    (void)user;
    free(memory);
}

static uint64_t probe_now_ms(void *user)
{
    (void)user;
    return 0u;
}

const h2_pal_mem_api_t *h2_bk3633_probe_mem_api(void)
{
    static const h2_pal_mem_vtable_t vtable = {
        .alloc = probe_alloc,
        .realloc = probe_realloc,
        .free = probe_free,
    };
    static const h2_pal_mem_api_t api = {
        .vtable = &vtable,
    };
    return &api;
}

h2_libco_t *h2_bk3633_probe_executor_create(void)
{
    h2_libco_t *executor = NULL;
    const h2_libco_config_t config = {
        .alloc = probe_alloc,
        .free = probe_free,
        .now_ms = probe_now_ms,
    };
    return h2_libco_create(&config, &executor) == H2_LIBCO_OK
        ? executor : NULL;
}

void h2_bk3633_probe_executor_destroy(h2_libco_t **executor)
{
    if (executor != NULL && *executor != NULL) {
        (void)h2_libco_destroy(executor);
    }
}

h2_pal_result_t h2_bk3633_probe_record_wake(void *user,
                                            uintptr_t wait_key)
{
    h2_libco_t *executor = user;
    return h2_libco_wake(executor, wait_key, H2_LIBCO_WAKE_ALL, NULL) ==
                   H2_LIBCO_OK
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t h2_bk3633_probe_wait_completion(
    void *user, uintptr_t wait_key, uint32_t timeout_ms)
{
    h2_libco_result_t result = h2_libco_wait(user, wait_key, timeout_ms);
    switch (result) {
    case H2_LIBCO_OK:
    case H2_LIBCO_WOKEN:
        return H2_PAL_OK;
    case H2_LIBCO_ERR_TIMEOUT:
        return H2_PAL_ERR_TIMEOUT;
    case H2_LIBCO_ERR_CANCELLED:
        return H2_PAL_EXIT;
    default:
        return H2_PAL_ERR_INVALID_STATE;
    }
}

void appm_init(void);

void flash_init(void)
{
}

void flash_read_data(uint8_t *data, uint32_t address, uint32_t length)
{
    (void)address;
    memset(data, 0xff, length);
}

int nvds_init(void)
{
    return s_nvds_result;
}

void rwip_init(uint8_t error)
{
    (void)error;
    appm_init();
    ++s_application_init_count;
}

void rwip_schedule(void)
{
    ++s_schedule_count;
}

h2_pal_result_t h2_bk3633_sdk_runtime_configure_rom_environment(
    const h2_bk3633_sdk_runtime_config_t *config)
{
    return config == NULL
        ? H2_PAL_ERR_INVALID_ARG
        : s_rom_environment_result;
}

int h2_bk3633_probe_application_init_count(void)
{
    return s_application_init_count;
}

int h2_bk3633_probe_schedule_count(void)
{
    return s_schedule_count;
}

void h2_bk3633_probe_set_nvds_result(int result)
{
    s_nvds_result = result;
}

void h2_bk3633_probe_set_rom_environment_result(h2_pal_result_t result)
{
    s_rom_environment_result = result;
}
