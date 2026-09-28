#ifndef H2_JSON_MOBILE_RUNNER_H
#define H2_JSON_MOBILE_RUNNER_H
#include "h2_runtime.h"
typedef struct h2_json_provider_factory {
    h2_pal_result_t (*create)(const h2_pal_mem_api_t *mem,
                              void **out_provider,
                              const h2_pal_json_api_t **out_api);
    h2_pal_result_t (*destroy)(void **provider);
} h2_json_provider_factory_t;
/* Borrow config APIs for one synchronous run, then retire Runtime before
 * invoking the supplied platform shutdown. The report path is caller-owned. */
int h2_json_mobile_run(h2_runtime_config_t config, const char *platform,
                         const char *version, const char *path,
                         const h2_json_provider_factory_t *factory,
                         int (*shutdown)(void));
#endif
