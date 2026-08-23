#include "h2_gizclaw_pal_e2e_access_point.h"

#include <string.h>

int h2_gizclaw_pal_e2e_access_point_parse(
    const char *value, h2_gizclaw_pal_e2e_access_point_t *out) {
    if (value == NULL || out == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (strcmp(value, "bj") == 0) {
        *out = H2_GIZCLAW_PAL_E2E_ACCESS_POINT_BJ;
        return H2_PAL_OK;
    }
    if (strcmp(value, "ap") == 0) {
        *out = H2_GIZCLAW_PAL_E2E_ACCESS_POINT_AP;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_INVALID_ARG;
}

const char *h2_gizclaw_pal_e2e_access_point_name(
    h2_gizclaw_pal_e2e_access_point_t access_point) {
    switch (access_point) {
        case H2_GIZCLAW_PAL_E2E_ACCESS_POINT_BJ:
            return "bj";
        case H2_GIZCLAW_PAL_E2E_ACCESS_POINT_AP:
            return "ap";
        default:
            return NULL;
    }
}

const char *h2_gizclaw_pal_e2e_access_point_endpoint(
    h2_gizclaw_pal_e2e_access_point_t access_point) {
    switch (access_point) {
        case H2_GIZCLAW_PAL_E2E_ACCESS_POINT_BJ:
            return "edge-bj-01.e2e.gizclaw.com:9821";
        case H2_GIZCLAW_PAL_E2E_ACCESS_POINT_AP:
            return "ap.e2e.gizclaw.com:9821";
        default:
            return NULL;
    }
}
