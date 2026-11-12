#ifndef H2_G99_MOSAICO_INPUT_H
#define H2_G99_MOSAICO_INPUT_H

/* G99 logical inputs on Mosaico. The final entry supplies the physical IDs
 * mapped to HOME, SCREEN, BATTERY and optional HAPTICS; POWER stays with the
 * hardware.
 * Mosaico has no UP, DOWN or OK keys, so Main takes
 * them from touch. An E2E image can add them as virtual push-edge Buttons
 * that only Runtime Test Control drives. Pure C over the board's periph API;
 * no SDK handles. */

#include "h2_runtime.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_g99_mosaico_input_config {
  h2_pal_periph_id_t home_periph_id;
  h2_pal_periph_id_t screen_periph_id;
  h2_pal_periph_id_t battery_periph_id;
  h2_pal_periph_id_t virtual_first_id;
  bool virtual_keys;
  h2_pal_periph_id_t haptics_periph_id;
} h2_g99_mosaico_input_config_t;

typedef struct h2_g99_mosaico_input {
  const h2_pal_periph_api_t* board;
  h2_g99_mosaico_input_config_t config;
  h2_pal_periph_api_t periph;
  h2_runtime_component_mapper_t mapper;
} h2_g99_mosaico_input_t;

/**
 * Prepares the periph API and component mapper to put in the Runtime config.
 * `board` (the board's periph API) and `input` are borrowed for the Runtime
 * lifetime. The configuration is copied. With `config->virtual_keys`, UP, DOWN
 * and OK map to three virtual Buttons starting at virtual_first_id.
 * A nonzero haptics_periph_id maps the optional PWM vibration motor.
 * Missing physical mappings return NOT_FOUND; invalid or colliding IDs return
 * INVALID_ARG. Provider failures propagate before the adapter is published.
 */
h2_pal_result_t h2_g99_mosaico_input_init(
    h2_g99_mosaico_input_t* input, const h2_pal_periph_api_t* board,
    const h2_g99_mosaico_input_config_t* config);

#ifdef __cplusplus
}
#endif

#endif
