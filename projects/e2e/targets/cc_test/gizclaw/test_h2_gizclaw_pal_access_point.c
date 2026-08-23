#include "h2_gizclaw_pal_e2e_access_point.h"

#include <stdio.h>
#include <string.h>

static int expect_invalid(const char *value) {
    h2_gizclaw_pal_e2e_access_point_t access_point = 0;
    return h2_gizclaw_pal_e2e_access_point_parse(value, &access_point) ==
           H2_PAL_ERR_INVALID_ARG;
}

int main(void) {
    h2_gizclaw_pal_e2e_access_point_t access_point = 0;
    if (h2_gizclaw_pal_e2e_access_point_parse("bj", &access_point) !=
            H2_PAL_OK ||
        access_point != H2_GIZCLAW_PAL_E2E_ACCESS_POINT_BJ ||
        strcmp(h2_gizclaw_pal_e2e_access_point_endpoint(access_point),
               "edge-bj-01.e2e.gizclaw.com:9821") != 0 ||
        h2_gizclaw_pal_e2e_access_point_parse("ap", &access_point) !=
            H2_PAL_OK ||
        access_point != H2_GIZCLAW_PAL_E2E_ACCESS_POINT_AP ||
        strcmp(h2_gizclaw_pal_e2e_access_point_endpoint(access_point),
               "ap.e2e.gizclaw.com:9821") != 0 ||
        !expect_invalid(NULL) || !expect_invalid("") ||
        !expect_invalid("AP") || !expect_invalid("beijing") ||
        !expect_invalid("e2e.gizclaw.com:9821") ||
        h2_gizclaw_pal_e2e_access_point_parse("ap", NULL) !=
            H2_PAL_ERR_INVALID_ARG ||
        h2_gizclaw_pal_e2e_access_point_name(0) != NULL ||
        h2_gizclaw_pal_e2e_access_point_endpoint(0) != NULL) {
        fprintf(stderr, "access point contract failed\n");
        return 1;
    }
    return 0;
}
