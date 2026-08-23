#ifndef H2_PAL_BUZZER_H
#define H2_PAL_BUZZER_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/hal/h2_pal_periph.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_BUZZER_VOLUME_PERCENT_MAX 100u

typedef h2_pal_periph_id_t h2_pal_buzzer_id_t;

/** Describes the portable operating range of one continuous-tone buzzer. */
typedef struct h2_pal_buzzer_info {
    h2_pal_buzzer_id_t id; /**< Physical peripheral identifier. */
    uint32_t min_frequency_hz; /**< Lowest supported non-zero frequency. */
    uint32_t max_frequency_hz; /**< Highest supported frequency. */
    uint8_t supports_volume; /**< Non-zero when logical volume is supported. */
    uint8_t reserved[3];
} h2_pal_buzzer_info_t;

typedef struct h2_pal_buzzer_vtable {
    h2_pal_result_t (*get_info)(
        void *user,
        h2_pal_buzzer_id_t id,
        h2_pal_buzzer_info_t *out_info);

    h2_pal_result_t (*start)(
        void *user,
        h2_pal_buzzer_id_t id,
        uint32_t frequency_hz,
        uint8_t volume_percent);

    h2_pal_result_t (*stop)(
        void *user,
        h2_pal_buzzer_id_t id);
} h2_pal_buzzer_vtable_t;

typedef struct h2_pal_buzzer_api {
    void *user;
    const h2_pal_buzzer_vtable_t *vtable;
} h2_pal_buzzer_api_t;

/** Query the verified continuous-frequency and logical-volume capability. */
static inline h2_pal_result_t h2_pal_buzzer_get_info(
    const h2_pal_buzzer_api_t *api,
    h2_pal_buzzer_id_t id,
    h2_pal_buzzer_info_t *out_info) {
    if (out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_info == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_info(api->user, id, out_info);
}

/**
 * Start or replace one continuous tone.
 *
 * The backend validates the requested frequency against get_info(). A zero
 * frequency or volume above 100 percent is rejected before dispatch.
 */
static inline h2_pal_result_t h2_pal_buzzer_start(
    const h2_pal_buzzer_api_t *api,
    h2_pal_buzzer_id_t id,
    uint32_t frequency_hz,
    uint8_t volume_percent) {
    if (frequency_hz == 0u ||
        volume_percent > H2_PAL_BUZZER_VOLUME_PERCENT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->start == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->start(
        api->user, id, frequency_hz, volume_percent);
}

/** Stop tone generation. Repeated calls must succeed and leave the buzzer silent. */
static inline h2_pal_result_t h2_pal_buzzer_stop(
    const h2_pal_buzzer_api_t *api,
    h2_pal_buzzer_id_t id) {
    if (api == NULL || api->vtable == NULL || api->vtable->stop == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->stop(api->user, id);
}

#ifdef __cplusplus
}
#endif

#endif
