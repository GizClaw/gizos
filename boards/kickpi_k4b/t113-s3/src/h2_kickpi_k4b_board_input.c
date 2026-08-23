#include "h2_kickpi_k4b_board_private.h"

#include "h2_linux_platform.h"

#define H2_KICKPI_K4B_PD14_LINE_OFFSET 110u

static const h2_pal_periph_single_button_payload_t s_pull_button = {
    .delivery = H2_PAL_BUTTON_DELIVERY_POLL_STATE,
};

static const h2_pal_periph_info_t s_kickpi_k4b_action_button = {
    .id = H2_KICKPI_K4B_PERIPH_ACTION_BUTTON,
    .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
    .name = "action_button",
    .payload = &s_pull_button,
    .payload_size = sizeof(s_pull_button),
};

static h2_pal_result_t kickpi_k4b_periph_list(void *user,
                                              h2_pal_periph_type_t type_filter,
                                              h2_pal_periph_cb_t callback,
                                              void *callback_user) {
  (void)user;
  if (callback == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (type_filter != H2_PAL_PERIPH_TYPE_ANY &&
      type_filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON) {
    return H2_PAL_OK;
  }
  return callback(callback_user, &s_kickpi_k4b_action_button);
}

static h2_pal_result_t kickpi_k4b_periph_get(void *user,
                                             h2_pal_periph_id_t periph_id,
                                             h2_pal_periph_info_t *out_info) {
  (void)user;
  if (out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (periph_id != H2_KICKPI_K4B_PERIPH_ACTION_BUTTON) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_info = s_kickpi_k4b_action_button;
  return H2_PAL_OK;
}

static const h2_pal_periph_vtable_t s_kickpi_k4b_periph_vtable = {
    .list = kickpi_k4b_periph_list,
    .get = kickpi_k4b_periph_get,
};

static const h2_pal_periph_api_t s_kickpi_k4b_periph_api = {
    .user = NULL,
    .vtable = &s_kickpi_k4b_periph_vtable,
};

h2_pal_result_t h2_kickpi_k4b_board_configure_input(void) {
  const h2_linux_gpio_button_config_t buttons[] = {{
      .periph_id = H2_KICKPI_K4B_PERIPH_ACTION_BUTTON,
      .chip_label = "pio",
      .line_offset = H2_KICKPI_K4B_PD14_LINE_OFFSET,
      .active_low = 1,
  }};
  h2_pal_result_t result = h2_linux_configure_gpio_buttons(
      buttons, sizeof(buttons) / sizeof(buttons[0]));
  if (result != H2_PAL_OK) {
    return result;
  }
  const h2_linux_evdev_touch_config_t touch = {
      .device_name = "gt9xxnew_ts",
      .width = H2_KICKPI_K4B_DISPLAY_WIDTH,
      .height = H2_KICKPI_K4B_DISPLAY_HEIGHT,
      .swap_xy = 0,
      .invert_x = 0,
      .invert_y = 0,
  };
  return h2_linux_configure_evdev_touch(&touch);
}

const h2_pal_periph_api_t *h2_kickpi_k4b_board_periph_api(void) {
  return &s_kickpi_k4b_periph_api;
}
