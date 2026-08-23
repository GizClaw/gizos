#ifndef H2_GIZCLAW_PAL_E2E_ACCESS_POINT_H
#define H2_GIZCLAW_PAL_E2E_ACCESS_POINT_H

#include "h2/pal/core/h2_pal_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_gizclaw_pal_e2e_access_point {
    H2_GIZCLAW_PAL_E2E_ACCESS_POINT_BJ = 1,
    H2_GIZCLAW_PAL_E2E_ACCESS_POINT_AP = 2,
} h2_gizclaw_pal_e2e_access_point_t;

int h2_gizclaw_pal_e2e_access_point_parse(
    const char *value, h2_gizclaw_pal_e2e_access_point_t *out);
const char *h2_gizclaw_pal_e2e_access_point_name(
    h2_gizclaw_pal_e2e_access_point_t access_point);
const char *h2_gizclaw_pal_e2e_access_point_endpoint(
    h2_gizclaw_pal_e2e_access_point_t access_point);

#ifdef __cplusplus
}
#endif

#endif
