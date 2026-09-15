#ifndef H2_BUTTON_SMOKE_H
#define H2_BUTTON_SMOKE_H

#include "h2_runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_button_smoke_button {
  h2_runtime_component_id_t component_id;
  const char *name;
} h2_button_smoke_button_t;

typedef struct h2_button_smoke_config {
  uint32_t width;
  uint32_t height;
  const h2_button_smoke_button_t *buttons;
  size_t button_count;
  int (*should_stop)(void *user);
  void *stop_user;
  void (*on_started)(void *user, h2_pal_result_t result);
  void *started_user;
} h2_button_smoke_config_t;

/** Run a display-backed Runtime Button event monitor. */
h2_pal_result_t h2_button_smoke_run(
    h2_runtime_t *runtime, const h2_button_smoke_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
