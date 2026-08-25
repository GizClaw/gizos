#include "board_config.h"
#include "h2_esp_layout_task_policy.h"

#include "h2_esp_board.h"
#include "h2_esp_board_config.h"
#include "h2_esp_platform_core.h"
#include "h2_lua_flappybird.h"

#include <stddef.h>

static h2_pal_result_t mapper_list(void *user, h2_runtime_component_t filter,
                                   h2_runtime_component_mapping_cb_t callback,
                                   void *callback_user) {
  h2_runtime_component_mapping_entry_t entry;
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_BUTTON)
    return H2_PAL_OK;
  entry = (h2_runtime_component_mapping_entry_t){
      .component_id = H2_LUA_FLAPPYBIRD_COMPONENT_BACK,
      .periph_id = H2_AMOLED_BOOT_BUTTON_ID,
  };
  return callback(callback_user, &entry);
}

static h2_pal_result_t mapper_get(void *user,
                                  h2_runtime_component_id_t component_id,
                                  h2_pal_periph_id_t *out_periph_id) {
  (void)user;
  if (out_periph_id == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (component_id != H2_LUA_FLAPPYBIRD_COMPONENT_BACK)
    return H2_PAL_ERR_NOT_FOUND;
  *out_periph_id = H2_AMOLED_BOOT_BUTTON_ID;
  return H2_PAL_OK;
}

static const h2_runtime_component_mapper_vtable_t s_mapper_vtable = {
    .list = mapper_list,
    .get_periph_id = mapper_get,
};

static const h2_runtime_component_mapper_t s_mapper = {
    .vtable = &s_mapper_vtable,
};

h2_pal_result_t
h2_lua_flappybird_amoled_runtime_config(h2_runtime_config_t *out_config) {
  h2_pal_result_t result = h2_esp_layout_task_policy_install();
  if (result != H2_PAL_OK)
    return result;
  result = h2_esp_board_runtime_config(out_config);
  if (result != H2_PAL_OK)
    return result;
  out_config->component_mapper = &s_mapper;
  out_config->mem = h2_esp_platform_psram_allocator();
  return H2_PAL_OK;
}

h2_pal_result_t h2_lua_flappybird_amoled_input_poll_config(
    h2_runtime_input_poll_config_t *out_config) {
  if (out_config == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_config = (h2_runtime_input_poll_config_t){
      .button_poll_interval_ms = 20u,
      .task_options = {.name = "lua_flappy_input"},
  };
  return H2_PAL_OK;
}
