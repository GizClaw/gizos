#include "h2_lvgl_button_internal.h"

#include <assert.h>

int main(void) {
  h2_runtime_t runtime_a = {0};
  h2_runtime_t runtime_b = {0};
  h2_lvgl_button_t first = {0};
  h2_lvgl_button_t second = {0};
  h2_lvgl_button_t third = {0};

  assert(h2_lvgl_button_registry_claim(&first, &runtime_a, 7u) == H2_PAL_OK);
  assert(h2_lvgl_button_registry_claim(&second, &runtime_a, 7u) ==
         H2_PAL_ERR_BUSY);
  assert(h2_lvgl_button_registry_claim(&first, &runtime_a, 8u) ==
         H2_PAL_ERR_BUSY);
  assert(h2_lvgl_button_registry_claim(&second, &runtime_a, 8u) == H2_PAL_OK);
  assert(h2_lvgl_button_registry_claim(&third, &runtime_b, 7u) == H2_PAL_OK);

  h2_lvgl_button_registry_release(&first);
  assert(h2_lvgl_button_registry_claim(&first, &runtime_a, 7u) == H2_PAL_OK);

  h2_lvgl_button_registry_release(&first);
  h2_lvgl_button_registry_release(&second);
  h2_lvgl_button_registry_release(&third);
  h2_lvgl_button_registry_release(&third);

  const h2_pal_periph_single_button_payload_t poll_payload = {
      .delivery = H2_PAL_BUTTON_DELIVERY_POLL_STATE,
  };
  const h2_pal_periph_single_button_payload_t push_payload = {
      .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
  };
  h2_pal_periph_info_t info = {
      .id = 7u,
      .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
      .payload = &poll_payload,
      .payload_size = sizeof(poll_payload),
  };
  assert(h2_lvgl_button_validate_periph(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lvgl_button_validate_periph(&info) == H2_PAL_ERR_INVALID_ARG);
  info.payload = &push_payload;
  assert(h2_lvgl_button_validate_periph(&info) == H2_PAL_OK);
  info.type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON;
  assert(h2_lvgl_button_validate_periph(&info) == H2_PAL_ERR_INVALID_ARG);
  return 0;
}
