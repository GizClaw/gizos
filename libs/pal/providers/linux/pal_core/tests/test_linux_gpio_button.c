#include "h2_linux_platform.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fake_gpio_reader_state {
  const char *expected_chip_label;
  uint32_t expected_line_offset;
  int expected_active_low;
  h2_pal_button_state_t state;
  h2_pal_result_t result;
  unsigned int calls;
} fake_gpio_reader_state_t;

static h2_pal_result_t fake_gpio_read(void *user, const char *chip_label,
                                      uint32_t line_offset, int active_low,
                                      h2_pal_button_state_t *out_state) {
  fake_gpio_reader_state_t *state = user;
  assert(strcmp(chip_label, state->expected_chip_label) == 0);
  assert(line_offset == state->expected_line_offset);
  assert(active_low == state->expected_active_low);
  ++state->calls;
  if (state->result == H2_PAL_OK) {
    *out_state = state->state;
  }
  return state->result;
}

static void test_configuration_and_reads(void) {
  char chip_label[] = "pio";
  const h2_linux_gpio_button_config_t config = {
      .periph_id = 7u,
      .chip_label = chip_label,
      .line_offset = 110u,
      .active_low = 1,
  };
  assert(h2_linux_configure_gpio_buttons(&config, 1u) == H2_PAL_OK);
  chip_label[0] = 'X';

  fake_gpio_reader_state_t reader = {
      .expected_chip_label = "pio",
      .expected_line_offset = 110u,
      .expected_active_low = 1,
      .state = H2_PAL_BUTTON_STATE_RELEASED,
      .result = H2_PAL_OK,
  };
  h2_linux_gpio_test_set_reader(fake_gpio_read, &reader);

  h2_pal_single_button_reading_t reading = {0};
  const h2_pal_button_api_t *api = h2_linux_gpio_button_api();
  assert(h2_pal_button_read_single_button(api, 7u, &reading) == H2_PAL_OK);
  assert(reading.id == 7u);
  assert(reading.state == H2_PAL_BUTTON_STATE_RELEASED);

  reader.state = H2_PAL_BUTTON_STATE_PRESSED;
  assert(h2_pal_button_read_single_button(api, 7u, &reading) == H2_PAL_OK);
  assert(reading.state == H2_PAL_BUTTON_STATE_PRESSED);
  assert(reader.calls == 2u);

  reader.result = H2_PAL_ERR_BUSY;
  assert(h2_pal_button_read_single_button(api, 7u, &reading) ==
         H2_PAL_ERR_BUSY);
  assert(reader.calls == 3u);

  assert(h2_pal_button_read_single_button(api, 8u, &reading) ==
         H2_PAL_ERR_NOT_FOUND);
  h2_pal_radio_button_group_reading_t group = {0};
  assert(h2_pal_button_read_radio_button_group(api, 7u, &group) ==
         H2_PAL_ERR_UNSUPPORTED);
}

static void test_invalid_configuration(void) {
  const h2_linux_gpio_button_config_t duplicate[] = {
      {.periph_id = 1u,
       .chip_label = "pio",
       .line_offset = 110u,
       .active_low = 1},
      {.periph_id = 1u,
       .chip_label = "pio",
       .line_offset = 111u,
       .active_low = 0},
  };
  const h2_linux_gpio_button_config_t invalid_id = {
      .periph_id = 0u,
      .chip_label = "pio",
      .line_offset = 110u,
      .active_low = 1,
  };
  const h2_linux_gpio_button_config_t invalid_active_low = {
      .periph_id = 1u,
      .chip_label = "pio",
      .line_offset = 110u,
      .active_low = 2,
  };
  assert(h2_linux_configure_gpio_buttons(NULL, 1u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_linux_configure_gpio_buttons(&invalid_id, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_linux_configure_gpio_buttons(&invalid_active_low, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_linux_configure_gpio_buttons(duplicate, 2u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_linux_configure_gpio_buttons(NULL, 0u) == H2_PAL_OK);
}

static void test_chip_node_name_discovery(void) {
  assert(h2_linux_gpio_test_is_chip_node_name("gpiochip0") != 0);
  assert(h2_linux_gpio_test_is_chip_node_name("gpiochip63") != 0);
  assert(h2_linux_gpio_test_is_chip_node_name("gpiochip64") != 0);
  assert(h2_linux_gpio_test_is_chip_node_name("gpiochip1048576") != 0);
  assert(h2_linux_gpio_test_is_chip_node_name("gpiochip") == 0);
  assert(h2_linux_gpio_test_is_chip_node_name("gpiochip7x") == 0);
  assert(h2_linux_gpio_test_is_chip_node_name("not-gpiochip7") == 0);
}

static void test_discovery_fails_closed_on_unreadable_candidates(void) {
  assert(h2_linux_gpio_test_discovery_result(
             1, H2_PAL_ERR_NOT_FOUND) == H2_PAL_OK);
  assert(h2_linux_gpio_test_discovery_result(
             0, H2_PAL_ERR_NOT_FOUND) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_linux_gpio_test_discovery_result(
             1, H2_PAL_ERR_UNAVAILABLE) == H2_PAL_ERR_UNAVAILABLE);
  assert(h2_linux_gpio_test_discovery_result(
             1, H2_PAL_ERR_IO) == H2_PAL_ERR_IO);
}

int main(void) {
  test_configuration_and_reads();
  test_invalid_configuration();
  test_chip_node_name_discovery();
  test_discovery_fails_closed_on_unreadable_candidates();
  h2_linux_gpio_test_set_reader(NULL, NULL);
  puts("linux gpio button tests passed");
  return 0;
}
