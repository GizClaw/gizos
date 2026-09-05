#include "h2_bk_ble_gatt_schema.h"

#include <assert.h>

int main(void) {
    /* One service with the three provisioning characteristics is accepted. */
    assert(h2_bk_ble_gatt_schema_accepts(1u, 3u));
    assert(h2_bk_ble_gatt_schema_accepts(1u, 1u));
    assert(h2_bk_ble_gatt_schema_accepts(H2_BK_BLE_MAX_GATT_SERVICES,
                                         H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE));

    /* Beyond the static schema the registration must be refused, not clipped. */
    assert(!h2_bk_ble_gatt_schema_accepts(
        1u, H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE + 1u));
    assert(!h2_bk_ble_gatt_schema_accepts(H2_BK_BLE_MAX_GATT_SERVICES + 1u, 1u));

    /* Services own disjoint, in-range slot ranges. */
    for (size_t service = 0u; service < H2_BK_BLE_MAX_GATT_SERVICES; ++service) {
        size_t first = h2_bk_ble_gatt_schema_first_slot(service);
        assert(first + H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE <=
               H2_BK_BLE_MAX_GATT_CHARACTERISTICS);
        if (service > 0u) {
            assert(first == h2_bk_ble_gatt_schema_first_slot(service - 1u) +
                                H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE);
        }
    }

    /* The attribute table must hold the service plus two attributes each. */
    assert(H2_BK_BLE_ATTR_COUNT ==
           1u + 2u * H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE);
    return 0;
}
