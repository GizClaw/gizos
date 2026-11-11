#ifndef H2_G99_SETTINGS_H
#define H2_G99_SETTINGS_H

/* User settings and their PAL preference persistence. */

#include "core/h2_pal_errors.h"
#include "os/h2_pal_pref.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_G99_SETTINGS_NAMESPACE "g99"
#define H2_G99_SETTINGS_STEP_PERCENT 10u
#define H2_G99_BRIGHTNESS_MIN_PERCENT 10u
#define H2_G99_SCREEN_TIMEOUT_COUNT 5u

typedef struct h2_g99_settings {
  uint8_t volume_percent;
  uint8_t brightness_percent;
  uint8_t screen_timeout_index;
} h2_g99_settings_t;

void h2_g99_settings_defaults(h2_g99_settings_t* settings);

/** Clamps every field to its valid range. */
void h2_g99_settings_normalize(h2_g99_settings_t* settings);

/** Screen timeout of a choice index; zero means never. */
uint32_t h2_g99_screen_timeout_ms(uint8_t index);

/** Applies one value step; returns the new value. */
uint8_t h2_g99_settings_step_volume(h2_g99_settings_t* settings, int direction);
uint8_t h2_g99_settings_step_brightness(h2_g99_settings_t* settings,
                                        int direction);

/**
 * Loads settings from `name_space` (NULL selects H2_G99_SETTINGS_NAMESPACE),
 * keeping defaults for absent or out-of-range keys. Returns OK when nothing
 * was stored. A NULL pref returns UNSUPPORTED with defaults written.
 */
h2_pal_result_t h2_g99_settings_load(const h2_pal_pref_api_t* pref,
                                     const char* name_space,
                                     h2_g99_settings_t* out);

/** Writes and commits every field to `name_space` (NULL as for load). */
h2_pal_result_t h2_g99_settings_save(const h2_pal_pref_api_t* pref,
                                     const char* name_space,
                                     const h2_g99_settings_t* settings);

#ifdef __cplusplus
}
#endif

#endif
