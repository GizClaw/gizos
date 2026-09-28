#ifndef H2_PAL_CORE_HOST_OBSERVER_H
#define H2_PAL_CORE_HOST_OBSERVER_H
#include "h2_pal_core_e2e.h"
#ifdef __cplusplus
extern "C" {
#endif
const h2_pal_mem_api_t *h2_pal_core_host_allocator(void);
const h2_pal_firmware_info_api_t *h2_pal_core_host_firmware_info(void);
const char *h2_pal_core_host_build_version(void);
h2_pal_result_t h2_pal_core_host_observer_start(void);
h2_pal_result_t h2_pal_core_host_observer_stop(void);
h2_pal_result_t h2_pal_core_host_resources(void *, h2_pal_core_resources_t *);
h2_pal_result_t h2_pal_core_host_stack(void *, size_t *);
h2_pal_result_t h2_pal_core_host_task_fault(void *, int);
#ifdef __cplusplus
}
#endif
#endif
