#ifndef H2_PAL_JSON_CONFORMANCE_H
#define H2_PAL_JSON_CONFORMANCE_H

#include "h2/pal/os/h2_pal_json.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stddef.h>

typedef h2_pal_result_t (*h2_pal_json_test_provider_create_fn)(
    const h2_pal_mem_api_t *mem,
    void **out_provider,
    const h2_pal_json_api_t **out_api);

typedef h2_pal_result_t (*h2_pal_json_test_provider_destroy_fn)(
    void **provider);

typedef struct h2_pal_json_test_provider {
    const char *name;
    h2_pal_json_test_provider_create_fn create;
    h2_pal_json_test_provider_destroy_fn destroy;
} h2_pal_json_test_provider_t;

typedef struct h2_pal_json_conformance_result {
    size_t peak_live_bytes;
} h2_pal_json_conformance_result_t;

void h2_pal_json_run_conformance(
    const h2_pal_json_test_provider_t *factory,
    h2_pal_json_conformance_result_t *out_result);

#endif
