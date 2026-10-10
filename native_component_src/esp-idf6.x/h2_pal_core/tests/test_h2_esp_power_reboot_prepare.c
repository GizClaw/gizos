/* Qualification assertions must also execute in optimized host builds. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_esp_power_reboot_prepare.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"
#include <assert.h>

#if CONFIG_IDF_TARGET_ESP32S31 && CONFIG_BT_NIMBLE_ENABLED && \
    !CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
static int stops, calls;
static h2_pal_result_t stop_result, safe_result;
static int cookie;
static h2_pal_result_t stop(void *user) {
    assert(user == &cookie);
    ++stops;
    return stop_result;
}
static const h2_pal_ble_vtable_t table = {.stop = stop};
static h2_pal_ble_t ble = {.user = &cookie, .vtable = &table};
h2_pal_ble_t *h2_esp_platform_ble(void) { return &ble; }
h2_pal_result_t h2_esp_platform_safe_call(h2_esp_platform_safe_call_cb_t cb,
    void *context, size_t context_size, size_t stack_depth) {
    ++calls;
    assert(context_size == sizeof(h2_pal_result_t));
    assert(stack_depth == 8192u);
    if (safe_result != H2_PAL_OK) return safe_result;
    /* Model the real worker's internal-context copy and copy-back. */
    h2_pal_result_t internal = *(h2_pal_result_t *)context;
    cb(&internal);
    *(h2_pal_result_t *)context = internal;
    return H2_PAL_OK;
}
#endif

int main(void) {
    const uint32_t loader = 0x20000u, app = 0x220000u;
    assert(h2_esp_power_boot_selection_is_current(app, app, app));
    assert(h2_esp_power_boot_selection_is_current(loader, loader, loader));
    /* App -> Loader selected, then reboot preparation fails: restore App. */
    assert(!h2_esp_power_boot_selection_is_current(app, loader, app));
    assert(!h2_esp_power_boot_selection_is_current(loader, app, loader));
    assert(!h2_esp_power_boot_selection_is_current(app, app, loader));
    assert(!h2_esp_power_boot_selection_is_current(app, loader, loader));
#if CONFIG_IDF_TARGET_ESP32S31 && CONFIG_BT_NIMBLE_ENABLED && \
    !CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    stop_result = safe_result = H2_PAL_OK;
    assert(h2_esp_power_reboot_prepare() == H2_PAL_OK);
    assert(calls == 1 && stops == 1);
    /* Already-stopped provider remains a successful preparation. */
    assert(h2_esp_power_reboot_prepare() == H2_PAL_OK);
    assert(calls == 2 && stops == 2);
    stop_result = H2_PAL_ERR_IO;
    assert(h2_esp_power_reboot_prepare() == H2_PAL_ERR_IO);
    assert(calls == 3 && stops == 3);
    safe_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_esp_power_reboot_prepare() == H2_PAL_ERR_TIMEOUT);
    assert(calls == 4 && stops == 3);
#else
    /* No BLE/safe-call definitions: a no-op profile must not link either. */
    assert(h2_esp_power_reboot_prepare() == H2_PAL_OK);
#endif
    return 0;
}
