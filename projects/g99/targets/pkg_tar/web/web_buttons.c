#include "web_buttons.h"

#include "h2_runtime_input_button.h"

#include <stdio.h>
#include <string.h>

#define WEB_PERIPH_BASE 1001u

typedef struct web_button {
  h2_runtime_component_id_t component_id;
  const char* name;
} web_button_t;

/* Board Button names match the logical Main components. */
static const web_button_t s_buttons[] = {
    {H2_G99_COMPONENT_HOME, "home"}, {H2_G99_COMPONENT_SCREEN, "screen"},
    {H2_G99_COMPONENT_UP, "up"},     {H2_G99_COMPONENT_DOWN, "down"},
    {H2_G99_COMPONENT_OK, "ok"},
};

#define WEB_BUTTON_COUNT (sizeof(s_buttons) / sizeof(s_buttons[0]))

static const h2_pal_periph_single_button_payload_t s_button_payload = {
    .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
};

static int index_of_component(h2_runtime_component_id_t component_id) {
  for (size_t i = 0u; i < WEB_BUTTON_COUNT; ++i)
    if (s_buttons[i].component_id == component_id) return (int)i;
  return -1;
}

static h2_pal_periph_info_t periph_info(size_t index) {
  h2_pal_periph_info_t info = {
      .id = WEB_PERIPH_BASE + (h2_pal_periph_id_t)index,
      .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
      .payload = &s_button_payload,
      .payload_size = sizeof(s_button_payload),
  };
  (void)snprintf(info.name, sizeof(info.name), "%s", s_buttons[index].name);
  return info;
}

static h2_pal_result_t periph_list(void* user, h2_pal_periph_type_t filter,
                                   h2_pal_periph_cb_t callback,
                                   void* callback_user) {
  (void)user;
  if (callback == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_PAL_PERIPH_TYPE_ANY &&
      filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON)
    return H2_PAL_OK;
  for (size_t i = 0u; i < WEB_BUTTON_COUNT; ++i) {
    const h2_pal_periph_info_t info = periph_info(i);
    const h2_pal_result_t rc = callback(callback_user, &info);
    if (rc != H2_PAL_OK) return rc;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t periph_get(void* user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t* out_info) {
  (void)user;
  if (out_info == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (id < WEB_PERIPH_BASE || id >= WEB_PERIPH_BASE + WEB_BUTTON_COUNT)
    return H2_PAL_ERR_NOT_FOUND;
  *out_info = periph_info(id - WEB_PERIPH_BASE);
  return H2_PAL_OK;
}

static h2_pal_result_t mapper_list(void* user, h2_runtime_component_t filter,
                                   h2_runtime_component_mapping_cb_t callback,
                                   void* callback_user) {
  (void)user;
  if (callback == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_NONE &&
      filter != H2_RUNTIME_COMPONENT_BUTTON)
    return H2_PAL_OK;
  for (size_t i = 0u; i < WEB_BUTTON_COUNT; ++i) {
    const h2_runtime_component_mapping_entry_t entry = {
        .component_id = s_buttons[i].component_id,
        .periph_id = WEB_PERIPH_BASE + (h2_pal_periph_id_t)i,
    };
    const h2_pal_result_t rc = callback(callback_user, &entry);
    if (rc != H2_PAL_OK) return rc;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t mapper_get(void* user,
                                  h2_runtime_component_id_t component_id,
                                  h2_pal_periph_id_t* out_id) {
  (void)user;
  if (out_id == NULL) return H2_PAL_ERR_INVALID_ARG;
  const int index = index_of_component(component_id);
  if (index < 0) return H2_PAL_ERR_NOT_FOUND;
  *out_id = WEB_PERIPH_BASE + (h2_pal_periph_id_t)index;
  return H2_PAL_OK;
}

static const h2_pal_periph_vtable_t s_periph_vtable = {
    .list = periph_list,
    .get = periph_get,
};
static const h2_pal_periph_api_t s_periph = {.vtable = &s_periph_vtable};
static const h2_runtime_component_mapper_vtable_t s_mapper_vtable = {
    .list = mapper_list,
    .get_periph_id = mapper_get,
};
static const h2_runtime_component_mapper_t s_mapper = {
    .vtable = &s_mapper_vtable,
};

const h2_pal_periph_api_t* h2_g99_web_periph_api(void) { return &s_periph; }

const h2_runtime_component_mapper_t* h2_g99_web_component_mapper(void) {
  return &s_mapper;
}

h2_runtime_component_id_t h2_g99_web_component(const char* name) {
  if (name == NULL) return H2_RUNTIME_COMPONENT_ID_NONE;
  if (strcmp(name, "ai") == 0) return H2_G99_COMPONENT_HOME;
  if (strcmp(name, "boot") == 0) return H2_G99_COMPONENT_SCREEN;
  for (size_t i = 0u; i < WEB_BUTTON_COUNT; ++i)
    if (strcmp(s_buttons[i].name, name) == 0) return s_buttons[i].component_id;
  return H2_RUNTIME_COMPONENT_ID_NONE;
}

h2_pal_result_t h2_g99_web_button_edge(h2_runtime_t* runtime,
                                       h2_runtime_component_id_t component_id,
                                       int pressed) {
  const int index = index_of_component(component_id);
  if (runtime == NULL || index < 0) return H2_PAL_ERR_INVALID_ARG;
  return h2_runtime_button_push_edge(
      runtime, WEB_PERIPH_BASE + (h2_pal_periph_id_t)index,
      pressed ? H2_RUNTIME_BUTTON_EDGE_DOWN : H2_RUNTIME_BUTTON_EDGE_UP);
}
