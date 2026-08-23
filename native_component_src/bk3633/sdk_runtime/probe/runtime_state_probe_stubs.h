#ifndef H2_BK3633_RUNTIME_STATE_PROBE_STUBS_H
#define H2_BK3633_RUNTIME_STATE_PROBE_STUBS_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2_libco.h"

int h2_bk3633_probe_application_init_count(void);
int h2_bk3633_probe_schedule_count(void);
void h2_bk3633_probe_set_nvds_result(int result);
void h2_bk3633_probe_set_rom_environment_result(h2_pal_result_t result);
h2_libco_t *h2_bk3633_probe_executor_create(void);
const h2_pal_mem_api_t *h2_bk3633_probe_mem_api(void);
void h2_bk3633_probe_executor_destroy(h2_libco_t **executor);
h2_pal_result_t h2_bk3633_probe_record_wake(void *user,
                                            uintptr_t wait_key);
h2_pal_result_t h2_bk3633_probe_wait_completion(
    void *user, uintptr_t wait_key, uint32_t timeout_ms);

#endif
