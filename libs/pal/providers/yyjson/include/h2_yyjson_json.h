#ifndef H2_YYJSON_JSON_H
#define H2_YYJSON_JSON_H

#include "h2/pal/os/h2_pal_json.h"
#include "h2/pal/os/h2_pal_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_yyjson_json h2_yyjson_json_t;

/**
 * @brief Create an independent yyjson-backed JSON PAL provider.
 *
 * The Memory PAL is copied by value and must remain usable until successful
 * provider destruction. Different provider instances do not share state.
 *
 * @param mem Complete borrowed memory capability; its backend must outlive the
 *     provider.
 * @param out_provider Required output, cleared before validation.
 * @return PAL result.
 */
h2_pal_result_t h2_yyjson_json_create(
    const h2_pal_mem_api_t *mem,
    h2_yyjson_json_t **out_provider);

/**
 * @brief Return the provider API, or the canonical unsupported API for NULL.
 * @param provider Borrowed provider instance, or NULL.
 * @return Borrowed API valid until successful provider destruction.
 */
const h2_pal_json_api_t *h2_yyjson_json_api(h2_yyjson_json_t *provider);

/**
 * @brief Destroy the provider and clear the caller handle.
 *
 * A NULL handle is a successful no-op. Live documents or serialized buffers
 * return H2_PAL_ERR_INVALID_STATE and leave the provider usable.
 *
 * @param provider Required pointer to the owned provider handle.
 * @return PAL result; success clears the caller handle.
 */
h2_pal_result_t h2_yyjson_json_destroy(h2_yyjson_json_t **provider);

#ifdef __cplusplus
}
#endif

#endif
