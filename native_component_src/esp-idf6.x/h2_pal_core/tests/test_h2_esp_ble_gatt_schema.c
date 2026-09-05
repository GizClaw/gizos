#include "h2_esp_ble_gatt_schema.h"

#include <assert.h>

int main(void) {
    /* One service with the three provisioning characteristics is accepted. */
    assert(h2_esp_ble_gatt_schema_accepts(1u, 3u));
    assert(h2_esp_ble_gatt_schema_accepts(1u, 1u));
    assert(h2_esp_ble_gatt_schema_accepts(H2_ESP_BLE_MAX_GATT_SERVICES,
                                          H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE));

    /* Beyond the static schema the registration must be refused, not clipped. */
    assert(!h2_esp_ble_gatt_schema_accepts(
        1u, H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE + 1u));
    assert(!h2_esp_ble_gatt_schema_accepts(H2_ESP_BLE_MAX_GATT_SERVICES + 1u, 1u));

    /*
     * Every slot is distinct and in range, including the second service's
     * later characteristics. Those are the ones a stale literal index table
     * left reading as zero, which routed their reads and writes to the first
     * service's first characteristic.
     */
    bool seen[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS] = { false };
    for (size_t service = 0u; service < H2_ESP_BLE_MAX_GATT_SERVICES; ++service) {
        for (size_t index = 0u;
             index < H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE; ++index) {
            size_t slot = h2_esp_ble_gatt_schema_slot(service, index);
            assert(slot < H2_ESP_BLE_MAX_GATT_CHARACTERISTICS);
            assert(!seen[slot]);
            seen[slot] = true;
        }
    }
    for (size_t slot = 0u; slot < H2_ESP_BLE_MAX_GATT_CHARACTERISTICS; ++slot) {
        assert(seen[slot]);
    }

    assert(h2_esp_ble_gatt_schema_slot(0u, 0u) == 0u);
    assert(h2_esp_ble_gatt_schema_slot(0u, 2u) == 2u);
    assert(h2_esp_ble_gatt_schema_slot(1u, 0u) == 3u);
    assert(h2_esp_ble_gatt_schema_slot(1u, 2u) == 5u);
    return 0;
}
