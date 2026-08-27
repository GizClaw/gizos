#ifndef H2_H2LOADER_WEB_STATUS_JSON_H
#define H2_H2LOADER_WEB_STATUS_JSON_H

#include "h2_h2loader_host.h"
#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Serialize one Host status into the Browser SDK's bounded JSON projection. */
h2_pal_result_t h2_h2loader_web_status_json_write(
    const h2_h2loader_host_status_t *status,
    char *out,
    size_t capacity,
    size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif
