#include "h2_esp_board_modem_ri.h"
#include <assert.h>
static bool service, attached;
static bool input_enabled, irq_enabled;
static gpio_int_type_t irq_type;
static unsigned service_calls, enable_calls;
static esp_err_t input_error;
static unsigned edges;
static void (*callback)(void*);
static esp_err_t install_error, add_error;
static bool rtc_held = true, rtc_mux = true;
static esp_err_t hold_error, mux_error;
esp_err_t rtc_gpio_hold_dis(gpio_num_t pin) {
  assert(pin == GPIO_NUM_0);
  if (hold_error) return hold_error;
  rtc_held = false;
  return ESP_OK;
}
esp_err_t rtc_gpio_deinit(gpio_num_t pin) {
  assert(pin == GPIO_NUM_0 && !rtc_held);
  if (mux_error) return mux_error;
  rtc_mux = false;
  return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t* config) {
  assert(!rtc_held && !rtc_mux);
  assert(config->pin_bit_mask == 1u && config->intr_type == GPIO_INTR_NEGEDGE);
  input_enabled = config->mode == GPIO_MODE_INPUT;
  irq_type = config->intr_type;
  return ESP_OK;
}
esp_err_t gpio_func_sel(gpio_num_t pin, uint32_t function) {
  assert(pin == GPIO_NUM_0 && function == PIN_FUNC_GPIO);
  assert(!rtc_held && !rtc_mux);
  return ESP_OK;
}
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) {
  assert(pin == GPIO_NUM_0 && mode == GPIO_MODE_INPUT);
  if (input_error != ESP_OK) return input_error;
  input_enabled = true;
  return ESP_OK;
}
esp_err_t gpio_pullup_dis(gpio_num_t pin) {
  assert(pin == GPIO_NUM_0);
  return ESP_OK;
}
esp_err_t gpio_pulldown_dis(gpio_num_t pin) {
  assert(pin == GPIO_NUM_0);
  return ESP_OK;
}
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type) {
  assert(pin == GPIO_NUM_0 && type == GPIO_INTR_NEGEDGE);
  irq_type = type;
  return ESP_OK;
}
esp_err_t gpio_intr_enable(gpio_num_t pin) {
  assert(pin == GPIO_NUM_0 && service && attached && input_enabled);
  ++enable_calls;
  irq_enabled = true;
  return ESP_OK;
}
esp_err_t gpio_install_isr_service(int flags) {
  ++service_calls;
  assert(flags == ESP_INTR_FLAG_SHARED);
  if (install_error) return install_error;
  if (service) return ESP_ERR_INVALID_STATE;
  service = true;
  return ESP_OK;
}
esp_err_t gpio_isr_handler_remove(gpio_num_t pin) {
  assert(pin == GPIO_NUM_0 && attached);
  attached = false;
  irq_enabled = false;
  callback = NULL;
  return ESP_OK;
}
esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void*),
                               void* arg) {
  assert(pin == GPIO_NUM_0 && arg == NULL && !attached);
  if (add_error) return add_error;
  attached = true;
  irq_enabled = true;
  callback = handler;
  return ESP_OK;
}
static void edge(void* arg) {
  (void)arg;
  ++edges;
}
static void physical_falling_edge(void) {
  if (!rtc_held && !rtc_mux && input_enabled && irq_enabled && attached &&
      irq_type == GPIO_INTR_NEGEDGE)
    callback(NULL);
}
int main(void) {
  bool installed = false;
  for (unsigned cycle = 0; cycle < 4u; ++cycle) {
    rtc_held = true;
    rtc_mux = true;
    assert(h2_board_modem_ri_start(&installed, edge) == ESP_OK);
    assert(installed && attached);
    callback(NULL);
    callback(NULL);
    assert(edges == (cycle + 1u) * 2u);
    assert(h2_board_modem_ri_start(&installed, edge) == ESP_OK);
    assert(gpio_isr_handler_remove(GPIO_NUM_0) == ESP_OK);
    installed = false;
  }
  assert(h2_board_modem_ri_start(&installed, edge) == ESP_OK);
  const unsigned calls_before_restore = service_calls;
  const unsigned edges_before_restore = edges;
  /* Retained/peripheral GPIO initialization can follow early registration.
   * A pulse during that window must also be recoverable as an active level. */
  rtc_held = rtc_mux = true;
  input_enabled = irq_enabled = false;
  physical_falling_edge();
  assert(edges == edges_before_restore);
  assert(h2_board_modem_ri_restore_input(installed) == ESP_OK);
  assert(service_calls == calls_before_restore && attached && callback == edge);
  bool was_low = false;
  assert(h2_board_modem_ri_level_take(false, true, &was_low));
  assert(!h2_board_modem_ri_level_take(false, true, &was_low));
  assert(!h2_board_modem_ri_level_take(false, false, &was_low));
  physical_falling_edge();
  assert(edges == edges_before_restore + 1u);
  assert(h2_board_modem_ri_level_take(true, true, &was_low));
  assert(!h2_board_modem_ri_level_take(false, true, &was_low));
  assert(h2_board_modem_ri_level_take(true, false, &was_low));
  assert(gpio_isr_handler_remove(GPIO_NUM_0) == ESP_OK);
  installed = false;
  const unsigned enabled_before = enable_calls;
  assert(h2_board_modem_ri_restore_input(false) == ESP_OK);
  assert(service_calls == calls_before_restore &&
         enable_calls == enabled_before);
  /* No service allocation is needed for physical-level polling either. */
  assert(h2_board_modem_ri_level_take(false, true, &was_low));
  input_error = ESP_FAIL;
  assert(h2_board_modem_ri_restore_input(false) == ESP_FAIL);
  input_error = ESP_OK;
  hold_error = ESP_FAIL;
  assert(h2_board_modem_ri_restore_input(false) == ESP_FAIL);
  assert(h2_board_modem_ri_start(&installed, edge) == ESP_FAIL && !installed);
  hold_error = ESP_OK;
  mux_error = ESP_FAIL;
  assert(h2_board_modem_ri_start(&installed, edge) == ESP_FAIL && !installed);
  mux_error = ESP_OK;
  install_error = ESP_FAIL;
  assert(h2_board_modem_ri_start(&installed, edge) == ESP_FAIL && !installed);
  install_error = ESP_OK;
  add_error = ESP_FAIL;
  assert(h2_board_modem_ri_start(&installed, edge) == ESP_FAIL && !installed);
  return 0;
}
