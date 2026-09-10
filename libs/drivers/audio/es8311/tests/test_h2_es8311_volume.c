#include "h2_es8311_volume.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

int main(void) {
    h2_es8311_volume_config_t legacy = {0};
    for (unsigned maximum = 1u; maximum <= 255u; ++maximum) {
        for (unsigned percent = 0u; percent <= 100u; ++percent) {
            assert(h2_es8311_volume_from_percent(&legacy, (uint8_t)maximum, percent) ==
                   percent * maximum / 100u);
        }
    }
    const uint32_t legacy_boundaries[] = {101u, 200u, 1000u, UINT32_MAX, UINT32_MAX / 255u};
    for (unsigned maximum = 1u; maximum <= 255u; ++maximum) {
        for (size_t i = 0u; i < sizeof(legacy_boundaries) / sizeof(legacy_boundaries[0]); ++i) {
            uint32_t scaled = legacy_boundaries[i] * maximum / 100u;
            assert(h2_es8311_volume_from_percent(&legacy, (uint8_t)maximum, legacy_boundaries[i]) ==
                   (scaled > 255u ? 255u : scaled));
        }
    }
    h2_es8311_volume_config_t curve = {
        .point_count = 5u,
        .points = {{1u, 120u}, {25u, 36u}, {50u, 12u}, {75u, 5u}, {100u, 0u}},
    };
    assert(h2_es8311_volume_is_valid(&curve, 0xb0u));
    assert(h2_es8311_volume_from_percent(&curve, 0xb0u, 0u) == 0u);
    assert(h2_es8311_volume_from_percent(&curve, 0xb0u, 1u) == 56u);
    assert(h2_es8311_volume_from_percent(&curve, 0xb0u, 25u) == 140u);
    assert(h2_es8311_volume_from_percent(&curve, 0xb0u, 50u) == 164u);
    assert(h2_es8311_volume_from_percent(&curve, 0xb0u, 60u) == 166u);
    assert(h2_es8311_volume_from_percent(&curve, 0xb0u, 100u) == 176u);
    assert(h2_es8311_volume_from_percent(&curve, 0xbfu, 50u) == 179u);
    for (unsigned maximum = 121u; maximum <= 255u; ++maximum) {
        uint8_t previous = 0u;
        for (unsigned percent = 1u; percent <= 100u; ++percent) {
            uint8_t value = h2_es8311_volume_from_percent(&curve, (uint8_t)maximum, percent);
            assert(value >= previous && value <= maximum && value > 0u);
            previous = value;
        }
    }
    assert(!h2_es8311_volume_is_valid(NULL, 176u));
    assert(!h2_es8311_volume_is_valid(&legacy, 0u));
    assert(!h2_es8311_volume_is_valid(&curve, 120u));
    assert(h2_es8311_volume_from_percent(&curve, 176u, 101u) == 0u);
    assert(h2_es8311_volume_from_percent(&curve, 176u, UINT32_MAX) == 0u);
    for (unsigned count = 1u; count <= 255u; ++count) {
        if (count == 5u) continue;
        h2_es8311_volume_config_t invalid = curve;
        invalid.point_count = (uint8_t)count;
        assert(!h2_es8311_volume_is_valid(&invalid, 176u));
    }
    for (unsigned i = 0u; i < 5u; ++i) {
        h2_es8311_volume_config_t invalid = curve;
        invalid.points[i].attenuation_half_db = 176u;
        assert(!h2_es8311_volume_is_valid(&invalid, 176u));
        assert(h2_es8311_volume_from_percent(&invalid, 176u, 100u) == 0u);
    }
    h2_es8311_volume_config_t invalid = curve;
    invalid.points[0].percent = 0u;
    assert(!h2_es8311_volume_is_valid(&invalid, 176u));
    invalid = curve;
    invalid.points[2].percent = 25u;
    assert(!h2_es8311_volume_is_valid(&invalid, 176u));
    invalid = curve;
    invalid.points[2].attenuation_half_db = 37u;
    assert(!h2_es8311_volume_is_valid(&invalid, 176u));
    invalid = curve;
    invalid.points[4].attenuation_half_db = 1u;
    assert(!h2_es8311_volume_is_valid(&invalid, 176u));
    h2_es8311_volume_config_t flat = {.point_count = 2u, .points = {{1u, 0u}, {100u, 0u}}};
    assert(h2_es8311_volume_is_valid(&flat, 1u));
    assert(h2_es8311_volume_from_percent(&flat, 1u, 50u) == 1u);
    h2_es8311_volume_config_t eight = {
        .point_count = 8u,
        .points = {{1u, 100u}, {10u, 80u}, {20u, 60u}, {30u, 40u},
                   {40u, 20u}, {50u, 10u}, {75u, 5u}, {100u, 0u}},
    };
    assert(h2_es8311_volume_is_valid(&eight, 101u));
    assert(h2_es8311_volume_from_percent(&eight, 101u, 10u) == 21u);
    invalid = curve;
    invalid.points[4].percent = 99u;
    assert(!h2_es8311_volume_is_valid(&invalid, 176u));
    invalid = curve;
    invalid.points[0].percent = 2u;
    assert(!h2_es8311_volume_is_valid(&invalid, 176u));
    return 0;
}
