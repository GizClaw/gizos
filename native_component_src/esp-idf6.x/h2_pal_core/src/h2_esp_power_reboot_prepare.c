#include "h2_esp_power_reboot_prepare.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S31 && CONFIG_BT_NIMBLE_ENABLED && \
    !CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"

static void stop_local_ble(void *context) {
    h2_pal_result_t *result = context;
    *result = h2_pal_ble_stop(h2_esp_platform_ble());
}
#endif

h2_pal_result_t h2_esp_power_reboot_prepare(void) {
#if CONFIG_IDF_TARGET_ESP32S31 && CONFIG_BT_NIMBLE_ENABLED && \
    !CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    /* Real S31 App -> Loader tests need full controller deinitialization,
     * rather than only the SDK's esp_restart shutdown callback. Use the
     * existing provider stop path on an internal stack, also for PSRAM tasks. */
    h2_pal_result_t result = H2_PAL_ERR_INVALID_STATE;
    h2_pal_result_t rc = h2_esp_platform_safe_call(
        stop_local_ble, &result, sizeof(result), 8192u);
    return rc != H2_PAL_OK ? rc : result;
#else
    return H2_PAL_OK;
#endif
}
