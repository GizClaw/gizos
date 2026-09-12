/* Board hardware hooks on app_host: prepare runs before
 * the Filesystem, configure_runtime installs its own Button peripheral and
 * component mapper (component 42 on peripheral 501), and the button hook
 * delivers the page's edges there. The App ends after one key press: both
 * its Down and Up edge must reach the hook and be accepted by the Runtime. */
#include "h2_web_app_host.h"

#include <stdio.h>
#include <string.h>

#define WEB_HARDWARE_COMPONENT 42u
#define WEB_HARDWARE_PERIPH 501u

typedef struct web_hardware {
  int prepared;
  int configured;
  volatile int edges;
  volatile int accepted;
} web_hardware_t;

static const h2_pal_periph_single_button_payload_t k_button_payload = {
    .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
};

static h2_pal_periph_info_t button_info(void) {
  h2_pal_periph_info_t info = {
      .id = WEB_HARDWARE_PERIPH,
      .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
      .payload = &k_button_payload,
      .payload_size = sizeof(k_button_payload),
  };
  (void)snprintf(info.name, sizeof(info.name), "action");
  return info;
}

static h2_pal_result_t periph_list(void *user, h2_pal_periph_type_t filter,
                                   h2_pal_periph_cb_t callback,
                                   void *callback_user) {
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_PAL_PERIPH_TYPE_ANY &&
      filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON)
    return H2_PAL_OK;
  const h2_pal_periph_info_t info = button_info();
  return callback(callback_user, &info);
}

static h2_pal_result_t periph_get(void *user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t *out_info) {
  (void)user;
  if (out_info == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (id != WEB_HARDWARE_PERIPH)
    return H2_PAL_ERR_NOT_FOUND;
  *out_info = button_info();
  return H2_PAL_OK;
}

static h2_pal_result_t mapper_list(void *user, h2_runtime_component_t filter,
                                   h2_runtime_component_mapping_cb_t callback,
                                   void *callback_user) {
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_NONE &&
      filter != H2_RUNTIME_COMPONENT_BUTTON)
    return H2_PAL_OK;
  const h2_runtime_component_mapping_entry_t entry = {
      .component_id = WEB_HARDWARE_COMPONENT,
      .periph_id = WEB_HARDWARE_PERIPH,
  };
  return callback(callback_user, &entry);
}

static h2_pal_result_t mapper_get(void *user,
                                  h2_runtime_component_id_t component_id,
                                  h2_pal_periph_id_t *out_id) {
  (void)user;
  if (out_id == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (component_id != WEB_HARDWARE_COMPONENT)
    return H2_PAL_ERR_NOT_FOUND;
  *out_id = WEB_HARDWARE_PERIPH;
  return H2_PAL_OK;
}

static const h2_pal_periph_vtable_t k_periph_vtable = {
    .list = periph_list,
    .get = periph_get,
};
static const h2_pal_periph_api_t k_periph = {.vtable = &k_periph_vtable};
static const h2_runtime_component_mapper_vtable_t k_mapper_vtable = {
    .list = mapper_list,
    .get_periph_id = mapper_get,
};
static const h2_runtime_component_mapper_t k_mapper = {
    .vtable = &k_mapper_vtable,
};

static h2_pal_result_t prepare(void *user, h2_web_platform_t *platform) {
  web_hardware_t *hardware = user;
  hardware->prepared = platform != NULL;
  return H2_PAL_OK;
}

static h2_pal_result_t configure_runtime(void *user,
                                         h2_runtime_config_t *config) {
  web_hardware_t *hardware = user;
  config->board = "web-hardware";
  config->periph = &k_periph;
  config->component_mapper = &k_mapper;
  hardware->configured = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t button(void *user, h2_runtime_t *runtime,
                              h2_runtime_component_id_t component_id,
                              int pressed) {
  web_hardware_t *hardware = user;
  if (component_id != WEB_HARDWARE_COMPONENT)
    return H2_PAL_ERR_NOT_FOUND;
  const h2_pal_result_t result = h2_runtime_button_push_edge(
      runtime, WEB_HARDWARE_PERIPH,
      pressed ? H2_RUNTIME_BUTTON_EDGE_DOWN : H2_RUNTIME_BUTTON_EDGE_UP);
  hardware->accepted += result == H2_PAL_OK;
  ++hardware->edges;
  return result;
}

static h2_pal_result_t run(h2_web_app_host_t *host, h2_runtime_t *runtime,
                           void *user) {
  web_hardware_t *hardware = user;
  printf("H2_WEB_HARDWARE prepared=%d configured=%d board=%s\n",
         hardware->prepared, hardware->configured, runtime->board);
  h2_pal_result_t result = h2_web_app_host_ready(host);
  /* One key press is one Down and one Up edge. */
  while (result == H2_PAL_OK && hardware->edges < 2 &&
         !h2_web_app_host_should_stop(host))
    result = h2_pal_time_sleep_ms(runtime->time, 10u);
  printf("H2_WEB_HARDWARE edges=%d accepted=%d\n", hardware->edges,
         hardware->accepted);
  if (result == H2_PAL_OK && hardware->accepted != hardware->edges)
    result = H2_PAL_ERR_UNAVAILABLE;
  return result;
}

int main(void) {
  static web_hardware_t hardware;
  static const h2_web_app_host_hardware_t hooks = {
      .user = &hardware,
      .prepare = prepare,
      .configure_runtime = configure_runtime,
      .button = button,
  };
  static const h2_web_app_host_button_t buttons[] = {
      {WEB_HARDWARE_COMPONENT, "Enter", NULL},
  };
  const h2_web_app_host_config_t config = {
      .name = "web-hardware",
      .buttons = buttons,
      .button_count = 1u,
      .hardware = &hooks,
  };
  return h2_web_app_host_run(&config, run, &hardware);
}
