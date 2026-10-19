#include "h2_pal_mqtt_e2e.h"
#include <assert.h>

int main(void) {
    h2_runtime_t runtime = {0};
    h2_pal_mqtt_e2e_config_t config = {.runtime = &runtime, .host = "127.0.0.1",
        .tcp_port = 1234u, .session = "missing", .topic_prefix = "h2/mqtt", .timeout_ms = 100u};
    h2_pal_mqtt_e2e_result_t result;
    assert(h2_pal_mqtt_e2e_run(NULL, &result) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_mqtt_e2e_run(&config, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_mqtt_e2e_run(&config, &result) != H2_PAL_OK);
    assert(result.selected == H2_PAL_MQTT_E2E_CASE_COUNT);
    assert(result.passed == 0u && result.failed == 0u && result.blocked == H2_PAL_MQTT_E2E_CASE_COUNT);
    for (unsigned index = 0u; index < H2_PAL_MQTT_E2E_CASE_COUNT; ++index) {
        assert(result.cases[index].blocked && !result.cases[index].passed);
    }
    config.smoke_only = 1;
    assert(h2_pal_mqtt_e2e_run(&config, &result) != H2_PAL_OK);
    assert(result.selected == 1u && result.blocked == 1u && result.passed == 0u);
    return 0;
}
