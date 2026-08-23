#include "h2_pal_host_fixture.h"

#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv) {
    assert(argc == 5);
    const h2_pal_host_fixture_config_t fixture_config = {
        .root_ca_path = argv[1],
        .wrong_ca_path = argv[2],
        .certificate_path = argv[3],
        .private_key_path = argv[4],
    };
    h2_pal_host_fixture_t *fixture = NULL;
    h2_runtime_t *runtime = NULL;
    h2_pal_e2e_config_t config;
    assert(h2_pal_host_fixture_create(&fixture_config, &fixture, &runtime,
                                      &config) ==
           H2_PAL_OK);
    h2_pal_e2e_result_t result;
    h2_pal_result_t run_result = h2_pal_e2e_run(runtime, &config, &result);
    for (size_t index = 0u; index < result.case_count; ++index) {
        fprintf(stderr, "H2_PAL_HOST_E2E case=%d result=%d\n",
                (int)result.cases[index].case_id,
                (int)result.cases[index].result);
    }
    fprintf(stderr,
            "H2_PAL_HOST_E2E selected=%zu passed=%zu failed=%zu "
            "cleanup=%d\n",
            result.selected, result.passed, result.failed,
            (int)result.cleanup_result);
    h2_pal_result_t destroy_result = h2_pal_host_fixture_destroy(fixture);
    assert(run_result == H2_PAL_OK);
    assert(result.selected == 19u);
    assert(result.passed == result.selected);
    assert(result.failed == 0u);
    assert(result.cleanup_result == H2_PAL_OK);
    assert(destroy_result == H2_PAL_OK);
    return 0;
}
