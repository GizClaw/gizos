#include "h2_lvgl_platform.h"

#include <stdbool.h>

static bool h2_lvgl_web_initialized;

int h2_lvgl_platform_init(const h2_lvgl_platform_config_t *config) {
  if (config == NULL || config->allocator == NULL || config->task_api == NULL ||
      config->sync_api == NULL || config->queue_api == NULL ||
      config->time_api == NULL || h2_lvgl_web_initialized) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_lvgl_web_initialized = true;
  return H2_PAL_OK;
}

void h2_lvgl_platform_deinit(void) { h2_lvgl_web_initialized = false; }
