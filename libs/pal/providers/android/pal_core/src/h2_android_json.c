#include "h2_android_platform.h"
#include "h2_yyjson_json.h"

h2_pal_result_t h2_android_json_provider_create(
    const h2_pal_mem_api_t *mem, void **out_provider,
    const h2_pal_json_api_t **out_api) {
    if (out_provider == NULL || out_api == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_provider = NULL;
    *out_api = NULL;
    h2_yyjson_json_t *provider = NULL;
    h2_pal_result_t rc = h2_yyjson_json_create(mem, &provider);
    if (rc != H2_PAL_OK) return rc;
    *out_api = h2_yyjson_json_api(provider);
    *out_provider = provider;
    return H2_PAL_OK;
}

h2_pal_result_t h2_android_json_provider_destroy(void **provider) {
    if (provider == NULL) return H2_PAL_ERR_INVALID_ARG;
    h2_yyjson_json_t *typed = *provider;
    h2_pal_result_t rc = h2_yyjson_json_destroy(&typed);
    if (rc == H2_PAL_OK) *provider = NULL;
    return rc;
}
