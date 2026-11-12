#include "h2_g99_settings.h"

#include <stddef.h>

static const uint32_t kTimeouts[H2_G99_SCREEN_TIMEOUT_COUNT] = {
    15000u, 30000u, 60000u, 300000u, 0u};

static const char kVolumeKey[] = "volume";
static const char kBrightnessKey[] = "brightness";
static const char kTimeoutKey[] = "screen_off";
static const char kHapticsKey[] = "haptics";

void h2_g99_settings_defaults(h2_g99_settings_t* settings) {
  if (settings == NULL) return;
  settings->volume_percent = 60u;
  settings->brightness_percent = 100u;
  settings->screen_timeout_index = 1u;
  settings->haptics_enabled = 0u;
}

void h2_g99_settings_normalize(h2_g99_settings_t* settings) {
  if (settings == NULL) return;
  if (settings->volume_percent > 100u) settings->volume_percent = 100u;
  if (settings->brightness_percent > 100u) settings->brightness_percent = 100u;
  if (settings->brightness_percent < H2_G99_BRIGHTNESS_MIN_PERCENT)
    settings->brightness_percent = H2_G99_BRIGHTNESS_MIN_PERCENT;
  if (settings->screen_timeout_index >= H2_G99_SCREEN_TIMEOUT_COUNT)
    settings->screen_timeout_index = 1u;
  if (settings->haptics_enabled > 1u) settings->haptics_enabled = 0u;
}

uint32_t h2_g99_screen_timeout_ms(uint8_t index) {
  return index < H2_G99_SCREEN_TIMEOUT_COUNT ? kTimeouts[index] : kTimeouts[1];
}

static uint8_t step(uint8_t value, int direction, uint8_t minimum) {
  int next =
      (int)value + (direction < 0 ? -1 : 1) * (int)H2_G99_SETTINGS_STEP_PERCENT;
  if (next < (int)minimum) next = (int)minimum;
  if (next > 100) next = 100;
  return (uint8_t)next;
}

uint8_t h2_g99_settings_step_volume(h2_g99_settings_t* settings,
                                    int direction) {
  if (settings == NULL) return 0u;
  settings->volume_percent = step(settings->volume_percent, direction, 0u);
  return settings->volume_percent;
}

uint8_t h2_g99_settings_step_brightness(h2_g99_settings_t* settings,
                                        int direction) {
  if (settings == NULL) return 0u;
  settings->brightness_percent = step(settings->brightness_percent, direction,
                                      H2_G99_BRIGHTNESS_MIN_PERCENT);
  return settings->brightness_percent;
}

static void read_u32(h2_pal_pref_namespace_t* ns, const char* key,
                     uint32_t limit, uint8_t* value) {
  uint32_t stored = 0u;
  if (ns->get_u32 != NULL && ns->get_u32(ns, key, &stored) == H2_PAL_OK &&
      stored <= limit)
    *value = (uint8_t)stored;
}

static const char* namespace_or_default(const char* name_space) {
  return name_space != NULL ? name_space : H2_G99_SETTINGS_NAMESPACE;
}

h2_pal_result_t h2_g99_settings_load(const h2_pal_pref_api_t* pref,
                                     const char* name_space,
                                     h2_g99_settings_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_g99_settings_defaults(out);
  if (pref == NULL) return H2_PAL_ERR_UNSUPPORTED;
  h2_pal_pref_namespace_t* ns = NULL;
  const int rc = h2_pal_pref_open(pref, namespace_or_default(name_space),
                                  H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  /* A namespace that was never written is a fresh device, not an error. */
  if (rc == H2_PAL_ERR_NOT_FOUND) return H2_PAL_OK;
  if (rc != H2_PAL_OK || ns == NULL) return (h2_pal_result_t)rc;
  read_u32(ns, kVolumeKey, 100u, &out->volume_percent);
  read_u32(ns, kBrightnessKey, 100u, &out->brightness_percent);
  read_u32(ns, kTimeoutKey, H2_G99_SCREEN_TIMEOUT_COUNT - 1u,
           &out->screen_timeout_index);
  read_u32(ns, kHapticsKey, 1u, &out->haptics_enabled);
  h2_g99_settings_normalize(out);
  return ns->close != NULL ? (h2_pal_result_t)ns->close(ns) : H2_PAL_OK;
}

h2_pal_result_t h2_g99_settings_save(const h2_pal_pref_api_t* pref,
                                     const char* name_space,
                                     const h2_g99_settings_t* settings) {
  if (pref == NULL || settings == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_g99_settings_t value = *settings;
  h2_g99_settings_normalize(&value);
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(pref, namespace_or_default(name_space),
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc != H2_PAL_OK || ns == NULL)
    return rc == H2_PAL_OK ? H2_PAL_ERR_IO : (h2_pal_result_t)rc;
  if (ns->set_u32 == NULL || ns->commit == NULL) {
    rc = H2_PAL_ERR_UNSUPPORTED;
  } else {
    rc = ns->set_u32(ns, kVolumeKey, value.volume_percent);
    if (rc == H2_PAL_OK)
      rc = ns->set_u32(ns, kBrightnessKey, value.brightness_percent);
    if (rc == H2_PAL_OK)
      rc = ns->set_u32(ns, kTimeoutKey, value.screen_timeout_index);
    if (rc == H2_PAL_OK)
      rc = ns->set_u32(ns, kHapticsKey, value.haptics_enabled);
    if (rc == H2_PAL_OK) rc = ns->commit(ns);
  }
  const int close_rc = ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
  return (h2_pal_result_t)(rc != H2_PAL_OK ? rc : close_rc);
}
