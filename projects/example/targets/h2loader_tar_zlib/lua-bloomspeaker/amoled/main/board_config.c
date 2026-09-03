#include "board_config.h"
#include "h2_esp_target_task_policy.h"

#include "h2_esp_board.h"
#include "h2_esp_board_config.h"
#include "h2_esp_platform_core.h"
#include "h2_bloomspeaker.h"

#include <stddef.h>

#define H2_BLOOMSPEAKER_DISPLAY_PCLK_HZ (80u * 1000u * 1000u)

typedef struct intercom_mapping {
  h2_runtime_component_id_t component_id;
  h2_pal_periph_id_t periph_id;
} intercom_mapping_t;

static const intercom_mapping_t s_mappings[] = {
    {H2_BLOOMSPEAKER_COMPONENT_POWER, H2_AMOLED_POWER_BUTTON_ID},
    {H2_BLOOMSPEAKER_COMPONENT_PAIR, H2_AMOLED_BOOT_BUTTON_ID},
};

static h2_pal_result_t mapper_list(void *user, h2_runtime_component_t filter,
                                   h2_runtime_component_mapping_cb_t callback,
                                   void *callback_user) {
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_BUTTON)
    return H2_PAL_OK;
  for (size_t index = 0u;
       index < sizeof(s_mappings) / sizeof(s_mappings[0]); ++index) {
    const h2_runtime_component_mapping_entry_t entry = {
        .component_id = s_mappings[index].component_id,
        .periph_id = s_mappings[index].periph_id,
    };
    h2_pal_result_t result = callback(callback_user, &entry);
    if (result != H2_PAL_OK)
      return result;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t mapper_get(void *user,
                                  h2_runtime_component_id_t component_id,
                                  h2_pal_periph_id_t *out_periph_id) {
  (void)user;
  if (out_periph_id == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t index = 0u;
       index < sizeof(s_mappings) / sizeof(s_mappings[0]); ++index) {
    if (component_id == s_mappings[index].component_id) {
      *out_periph_id = s_mappings[index].periph_id;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static const h2_runtime_component_mapper_vtable_t s_mapper_vtable = {
    .list = mapper_list,
    .get_periph_id = mapper_get,
};

static const h2_runtime_component_mapper_t s_mapper = {
    .vtable = &s_mapper_vtable,
};

h2_pal_result_t h2_bloomspeaker_amoled_runtime_config(
    h2_runtime_config_t *out_config) {
  h2_pal_result_t result = h2_esp_target_task_policy_install();
  if (result != H2_PAL_OK)
    return result;
  result = h2_esp_board_display_configure(
      &(h2_esp_board_display_config_t){
          .pclk_hz = H2_BLOOMSPEAKER_DISPLAY_PCLK_HZ,
      });
  if (result != H2_PAL_OK)
    return result;
  result = h2_esp_board_audio_configure(
      &(h2_esp_board_audio_config_t){
          .i2s_dma_desc_num = 4u,
          .i2s_dma_frame_num = 240u,
          .mic_gain_db = 12u,
          .mic_queue_frames = 16u,
          .aggressive_aec_nlp = 1,
      });
  if (result != H2_PAL_OK)
    return result;
  result = h2_esp_board_runtime_config(out_config);
  if (result != H2_PAL_OK)
    return result;
  out_config->component_mapper = &s_mapper;
  out_config->mem = h2_esp_platform_psram_allocator();
  return H2_PAL_OK;
}

h2_pal_result_t h2_bloomspeaker_amoled_input_poll_config(
    h2_runtime_input_poll_config_t *out_config) {
  if (out_config == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_config = (h2_runtime_input_poll_config_t){
      .button_poll_interval_ms = 20u,
  };
  return H2_PAL_OK;
}
