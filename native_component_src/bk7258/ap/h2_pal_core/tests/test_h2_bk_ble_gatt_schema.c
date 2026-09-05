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

    /*
     * EtherMind spends one entry per characteristic plus one for a CCCD, so
     * the worst case is two each. The provisioning service is one write-only
     * and two notifying characteristics: 1 + 1 + 2 + 2 = 6 entries.
     */
    assert(H2_BK_BLE_ATTR_COUNT ==
           1u + 2u * H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE);
    assert(H2_BK_BLE_ATTR_COUNT >= 6u);

    /*
     * The legacy database spells out declaration, value and CCCD for every
     * characteristic whether it notifies or not, so it needs three each. A
     * third characteristic used to write past the fixed two-characteristic
     * table.
     */
    assert(H2_BK_BLE_LEGACY_ATTR_COUNT ==
           1u + 3u * H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE);
    for (size_t i = 0u; i < H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE; ++i) {
        assert(h2_bk_ble_legacy_declaration_index(i) < H2_BK_BLE_LEGACY_ATTR_COUNT);
        assert(h2_bk_ble_legacy_value_slot(i) < H2_BK_BLE_LEGACY_ATTR_COUNT);
        assert(h2_bk_ble_legacy_cccd_slot(i) < H2_BK_BLE_LEGACY_ATTR_COUNT);
        assert(h2_bk_ble_legacy_declaration_index(i) != H2_BK_BLE_LEGACY_SERVICE_INDEX);
    }
    /* The third characteristic keeps the layout the old enum spelled out. */
    assert(h2_bk_ble_legacy_declaration_index(0u) == 1u);
    assert(h2_bk_ble_legacy_value_slot(0u) == 2u);
    assert(h2_bk_ble_legacy_cccd_slot(0u) == 3u);
    assert(h2_bk_ble_legacy_declaration_index(1u) == 4u);
    assert(h2_bk_ble_legacy_value_slot(1u) == 5u);
    assert(h2_bk_ble_legacy_cccd_slot(1u) == 6u);
    assert(h2_bk_ble_legacy_value_slot(2u) == 8u);
    assert(h2_bk_ble_legacy_cccd_slot(2u) == 9u);

    /* Reverse lookup only claims attributes the service actually registered. */
    assert(h2_bk_ble_legacy_characteristic_from_value(2u, 3u) == 0);
    assert(h2_bk_ble_legacy_characteristic_from_value(5u, 3u) == 1);
    assert(h2_bk_ble_legacy_characteristic_from_value(8u, 3u) == 2);
    assert(h2_bk_ble_legacy_characteristic_from_value(8u, 2u) == -1);
    assert(h2_bk_ble_legacy_characteristic_from_value(1u, 3u) == -1);
    assert(h2_bk_ble_legacy_characteristic_from_cccd(3u, 3u) == 0);
    assert(h2_bk_ble_legacy_characteristic_from_cccd(9u, 3u) == 2);
    assert(h2_bk_ble_legacy_characteristic_from_cccd(9u, 2u) == -1);
    assert(h2_bk_ble_legacy_characteristic_from_cccd(2u, 3u) == -1);
    return 0;
}
