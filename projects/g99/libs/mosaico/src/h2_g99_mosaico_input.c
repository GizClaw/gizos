#include "h2_g99_mosaico_input.h"

#include "h2_g99.h"

#include <stdio.h>
#include <string.h>

typedef struct mapping {
  h2_runtime_component_t kind;
  h2_runtime_component_id_t component_id;
  /* Present only with virtual keys. */
  bool virtual_key;
} mapping_t;

static const mapping_t kMappings[] = {
    {H2_RUNTIME_COMPONENT_BUTTON, H2_G99_COMPONENT_HOME, false},
    {H2_RUNTIME_COMPONENT_BUTTON, H2_G99_COMPONENT_SCREEN, false},
    {H2_RUNTIME_COMPONENT_BATTERY, H2_G99_COMPONENT_BATTERY, false},
    {H2_RUNTIME_COMPONENT_BUTTON, H2_G99_COMPONENT_UP, true},
    {H2_RUNTIME_COMPONENT_BUTTON, H2_G99_COMPONENT_DOWN, true},
    {H2_RUNTIME_COMPONENT_BUTTON, H2_G99_COMPONENT_OK, true},
};

#define MAPPING_COUNT (sizeof(kMappings) / sizeof(kMappings[0]))
#define VIRTUAL_COUNT 3u

static const char* const kVirtualNames[VIRTUAL_COUNT] = {"e2e_up", "e2e_down",
                                                         "e2e_ok"};

/* Pushed edges only: nothing polls a virtual key. */
static const h2_pal_periph_single_button_payload_t kVirtualPayload = {
    .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
};

static bool is_virtual(const h2_g99_mosaico_input_t* input,
                       h2_pal_periph_id_t id) {
  return input->config.virtual_keys && id >= input->config.virtual_first_id &&
         id - input->config.virtual_first_id < VIRTUAL_COUNT;
}

static h2_pal_periph_id_t mapped_id(const h2_g99_mosaico_input_t* input,
                                    size_t index) {
  if (index == 0u) return input->config.home_periph_id;
  if (index == 1u) return input->config.screen_periph_id;
  if (index == 2u) return input->config.battery_periph_id;
  return input->config.virtual_first_id + (h2_pal_periph_id_t)(index - 3u);
}

static h2_pal_periph_info_t virtual_info(const h2_g99_mosaico_input_t* input,
                                         h2_pal_periph_id_t id) {
  h2_pal_periph_info_t info;
  memset(&info, 0, sizeof(info));
  info.id = id;
  info.type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON;
  info.payload = &kVirtualPayload;
  info.payload_size = sizeof(kVirtualPayload);
  (void)snprintf(info.name, sizeof(info.name), "%s",
                 kVirtualNames[id - input->config.virtual_first_id]);
  return info;
}

static h2_pal_result_t periph_list(void* user, h2_pal_periph_type_t filter,
                                   h2_pal_periph_cb_t callback,
                                   void* callback_user) {
  const h2_g99_mosaico_input_t* input = user;
  if (callback == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc =
      h2_pal_periph_list(input->board, filter, callback, callback_user);
  if (rc != H2_PAL_OK || !input->config.virtual_keys) return rc;
  if (filter != H2_PAL_PERIPH_TYPE_ANY &&
      filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON)
    return H2_PAL_OK;
  for (uint32_t i = 0u; i < VIRTUAL_COUNT; ++i) {
    const h2_pal_periph_info_t info =
        virtual_info(input, input->config.virtual_first_id + i);
    rc = callback(callback_user, &info);
    if (rc != H2_PAL_OK) return rc;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t periph_get(void* user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t* out_info) {
  const h2_g99_mosaico_input_t* input = user;
  if (out_info == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (is_virtual(input, id)) {
    *out_info = virtual_info(input, id);
    return H2_PAL_OK;
  }
  return h2_pal_periph_get(input->board, id, out_info);
}

static h2_pal_result_t mapper_list(void* user, h2_runtime_component_t filter,
                                   h2_runtime_component_mapping_cb_t callback,
                                   void* callback_user) {
  const h2_g99_mosaico_input_t* input = user;
  if (callback == NULL) return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < MAPPING_COUNT; ++i) {
    const mapping_t* m = &kMappings[i];
    if (m->virtual_key && !input->config.virtual_keys) continue;
    if (filter != H2_RUNTIME_COMPONENT_NONE && filter != m->kind) continue;
    const h2_runtime_component_mapping_entry_t entry = {m->component_id,
                                                        mapped_id(input, i)};
    const h2_pal_result_t rc = callback(callback_user, &entry);
    if (rc != H2_PAL_OK) return rc;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t mapper_get(void* user,
                                  h2_runtime_component_id_t component_id,
                                  h2_pal_periph_id_t* out_id) {
  const h2_g99_mosaico_input_t* input = user;
  if (out_id == NULL) return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < MAPPING_COUNT; ++i) {
    const mapping_t* m = &kMappings[i];
    if (m->component_id != component_id) continue;
    if (m->virtual_key && !input->config.virtual_keys) break;
    *out_id = mapped_id(input, i);
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static const h2_pal_periph_vtable_t kPeriphVtable = {periph_list, periph_get};
static const h2_runtime_component_mapper_vtable_t kMapperVtable = {mapper_list,
                                                                   mapper_get};

h2_pal_result_t h2_g99_mosaico_input_init(
    h2_g99_mosaico_input_t* input, const h2_pal_periph_api_t* board,
    const h2_g99_mosaico_input_config_t* config) {
  if (input == NULL || board == NULL || config == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_periph_id_t ids[] = {config->home_periph_id,
                                    config->screen_periph_id,
                                    config->battery_periph_id};
  for (size_t index = 0u; index < 3u; ++index) {
    if (ids[index] == 0u) return H2_PAL_ERR_INVALID_ARG;
    for (size_t previous = 0u; previous < index; ++previous)
      if (ids[index] == ids[previous]) return H2_PAL_ERR_INVALID_ARG;
    h2_pal_periph_info_t info;
    const h2_pal_result_t rc = h2_pal_periph_get(board, ids[index], &info);
    if (rc != H2_PAL_OK) return rc;
  }
  if (config->virtual_keys) {
    if (config->virtual_first_id == 0u ||
        config->virtual_first_id > UINT32_MAX - (VIRTUAL_COUNT - 1u))
      return H2_PAL_ERR_INVALID_ARG;
    for (uint32_t index = 0u; index < VIRTUAL_COUNT; ++index) {
      h2_pal_periph_info_t info;
      const h2_pal_result_t rc =
          h2_pal_periph_get(board, config->virtual_first_id + index, &info);
      if (rc != H2_PAL_ERR_NOT_FOUND)
        return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_ARG : rc;
    }
  }
  memset(input, 0, sizeof(*input));
  input->board = board;
  input->config = *config;
  input->periph.user = input;
  input->periph.vtable = &kPeriphVtable;
  input->mapper.user = input;
  input->mapper.vtable = &kMapperVtable;
  return H2_PAL_OK;
}
