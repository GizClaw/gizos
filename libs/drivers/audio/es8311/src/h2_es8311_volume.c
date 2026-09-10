#include "h2_es8311_volume.h"

#include <stddef.h>

int h2_es8311_volume_is_valid(const h2_es8311_volume_config_t *config,
                            uint8_t max_register) {
    if (config == NULL || max_register == 0u) {
        return 0;
    }
    if (config->point_count == 0u) {
        return 1;
    }
    if (config->point_count < 2u || config->point_count > H2_ES8311_VOLUME_MAX_POINTS ||
        config->points[0].percent != 1u ||
        config->points[config->point_count - 1u].percent != 100u ||
        config->points[config->point_count - 1u].attenuation_half_db != 0u) {
        return 0;
    }
    for (uint8_t i = 0u; i < config->point_count; ++i) {
        if (config->points[i].attenuation_half_db >= max_register ||
            (i > 0u && (config->points[i].percent <= config->points[i - 1u].percent ||
                       config->points[i].attenuation_half_db >
                           config->points[i - 1u].attenuation_half_db))) {
            return 0;
        }
    }
    return 1;
}

uint8_t h2_es8311_volume_from_percent(const h2_es8311_volume_config_t *config,
                                    uint8_t max_register, uint32_t percent) {
    if (!h2_es8311_volume_is_valid(config, max_register)) {
        return 0u;
    }
    if (config->point_count == 0u) {
        const uint32_t scaled = (percent * (uint32_t)max_register) / 100u;
        return scaled > 0xffu ? 0xffu : (uint8_t)scaled;
    }
    if (percent > 100u || percent == 0u) {
        return 0u;
    }
    for (uint8_t i = 1u; i < config->point_count; ++i) {
        const h2_es8311_volume_point_t *left = &config->points[i - 1u];
        const h2_es8311_volume_point_t *right = &config->points[i];
        if (percent <= right->percent) {
            uint32_t rise = left->attenuation_half_db - right->attenuation_half_db;
            return (uint8_t)(max_register - left->attenuation_half_db +
                rise * (percent - left->percent) / (right->percent - left->percent));
        }
    }
    return max_register;
}
