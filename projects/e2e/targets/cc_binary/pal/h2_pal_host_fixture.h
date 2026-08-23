#ifndef H2_PAL_HOST_FIXTURE_H
#define H2_PAL_HOST_FIXTURE_H

#include "h2_pal_e2e.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_pal_host_fixture h2_pal_host_fixture_t;

typedef struct h2_pal_host_fixture_config {
    const char *root_ca_path;
    const char *wrong_ca_path;
    const char *certificate_path;
    const char *private_key_path;
} h2_pal_host_fixture_config_t;

h2_pal_result_t h2_pal_host_fixture_create(
    const h2_pal_host_fixture_config_t *fixture_config,
    h2_pal_host_fixture_t **out_fixture,
    h2_runtime_t **out_runtime,
    h2_pal_e2e_config_t *out_config);

h2_pal_result_t h2_pal_host_fixture_destroy(
    h2_pal_host_fixture_t *fixture);

#ifdef __cplusplus
}
#endif

#endif
