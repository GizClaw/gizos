#include "h2_lvgl_button.h"
#include "h2_lvgl_button_internal.h"

#define H2_LVGL_BUTTON_MAX 32u

static h2_lvgl_button_t *s_button_bindings[H2_LVGL_BUTTON_MAX];

h2_pal_result_t h2_lvgl_button_registry_claim(
    h2_lvgl_button_t *binding,
    h2_runtime_t *runtime,
    h2_pal_periph_id_t periph_id) {
  if (binding == NULL || runtime == NULL || periph_id == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  size_t free_index = H2_LVGL_BUTTON_MAX;
  for (size_t index = 0u; index < H2_LVGL_BUTTON_MAX; ++index) {
    h2_lvgl_button_t *current = s_button_bindings[index];
    if (current == NULL) {
      if (free_index == H2_LVGL_BUTTON_MAX) {
        free_index = index;
      }
      continue;
    }
    if (current == binding ||
        (current->runtime == runtime && current->periph_id == periph_id)) {
      return H2_PAL_ERR_BUSY;
    }
  }
  if (free_index == H2_LVGL_BUTTON_MAX) {
    return H2_PAL_ERR_NO_SPACE;
  }
  binding->runtime = runtime;
  binding->periph_id = periph_id;
  binding->last_result = H2_PAL_OK;
  binding->pressed = 0;
  s_button_bindings[free_index] = binding;
  return H2_PAL_OK;
}

void h2_lvgl_button_registry_release(h2_lvgl_button_t *binding) {
  if (binding == NULL) {
    return;
  }
  for (size_t index = 0u; index < H2_LVGL_BUTTON_MAX; ++index) {
    if (s_button_bindings[index] == binding) {
      s_button_bindings[index] = NULL;
      break;
    }
  }
}

h2_pal_result_t h2_lvgl_button_validate_periph(
    const h2_pal_periph_info_t *info) {
  if (info == NULL || info->type != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON ||
      info->payload == NULL ||
      info->payload_size < sizeof(h2_pal_periph_single_button_payload_t)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_pal_periph_single_button_payload_t *payload =
      (const h2_pal_periph_single_button_payload_t *)info->payload;
  return payload->delivery == H2_PAL_BUTTON_DELIVERY_PUSH_EDGE
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_ARG;
}

static void lvgl_button_event(lv_event_t *event) {
  h2_lvgl_button_t *binding = lv_event_get_user_data(event);
  if (binding == NULL || binding->runtime == NULL) {
    return;
  }
  const lv_event_code_t code = lv_event_get_code(event);
  h2_runtime_button_edge_t edge = H2_RUNTIME_BUTTON_EDGE_DOWN;
  int has_edge = 0;
  if (code == LV_EVENT_PRESSED && !binding->pressed) {
    binding->pressed = 1;
    edge = H2_RUNTIME_BUTTON_EDGE_DOWN;
    has_edge = 1;
  } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
              code == LV_EVENT_DELETE) &&
             binding->pressed) {
    binding->pressed = 0;
    edge = H2_RUNTIME_BUTTON_EDGE_UP;
    has_edge = 1;
  }
  if (has_edge != 0) {
    binding->last_result = h2_runtime_button_push_edge(
        binding->runtime, binding->periph_id, edge);
  }
  if (code == LV_EVENT_DELETE) {
    h2_lvgl_button_registry_release(binding);
    binding->runtime = NULL;
    binding->periph_id = 0u;
  }
}

h2_pal_result_t h2_lvgl_button_bind(
    h2_lvgl_button_t *binding,
    h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    lv_obj_t *object) {
  if (binding == NULL || runtime == NULL ||
      component_id == H2_RUNTIME_COMPONENT_ID_NONE || object == NULL ||
      runtime->periph == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }

  h2_pal_periph_id_t periph_id = 0u;
  h2_pal_result_t result =
      h2_runtime_periph_id(runtime, component_id, &periph_id);
  h2_pal_periph_info_t info = {0};
  if (result == H2_PAL_OK) {
    result = h2_pal_periph_get(runtime->periph, periph_id, &info);
  }
  if (result != H2_PAL_OK) {
    return result;
  }
  result = h2_lvgl_button_validate_periph(&info);
  if (result != H2_PAL_OK) {
    return result;
  }

  result = h2_lvgl_button_registry_claim(binding, runtime, periph_id);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (lv_obj_add_event_cb(object, lvgl_button_event, LV_EVENT_ALL, binding) ==
      NULL) {
    h2_lvgl_button_registry_release(binding);
    binding->runtime = NULL;
    binding->periph_id = 0u;
    return H2_PAL_ERR_NO_MEMORY;
  }
  return H2_PAL_OK;
}
