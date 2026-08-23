#include "h2_linux_platform.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fake_reader_state {
  const char *expected_device_name;
  uint16_t expected_key_code;
  h2_pal_button_state_t state;
  h2_pal_result_t result;
  unsigned int calls;
} fake_reader_state_t;

static h2_pal_result_t fake_read(void *user, const char *device_name,
                                 uint16_t key_code,
                                 h2_pal_button_state_t *out_state) {
  fake_reader_state_t *state = user;
  assert(strcmp(device_name, state->expected_device_name) == 0);
  assert(key_code == state->expected_key_code);
  ++state->calls;
  if (state->result == H2_PAL_OK) {
    *out_state = state->state;
  }
  return state->result;
}

static void test_configuration_and_reads(void) {
  char device_name[] = "sunxi-gpadc0";
  const h2_linux_evdev_button_config_t config = {
      .periph_id = 7u,
      .device_name = device_name,
      .key_code = 373u,
  };
  assert(h2_linux_configure_evdev_buttons(&config, 1u) == H2_PAL_OK);
  device_name[0] = 'X';

  fake_reader_state_t reader = {
      .expected_device_name = "sunxi-gpadc0",
      .expected_key_code = 373u,
      .state = H2_PAL_BUTTON_STATE_PRESSED,
      .result = H2_PAL_OK,
  };
  h2_linux_evdev_test_set_reader(fake_read, &reader);

  h2_pal_single_button_reading_t reading = {0};
  const h2_pal_button_api_t *api = h2_linux_evdev_button_api();
  assert(h2_pal_button_read_single_button(api, 7u, &reading) == H2_PAL_OK);
  assert(reading.id == 7u);
  assert(reading.state == H2_PAL_BUTTON_STATE_PRESSED);

  reader.state = H2_PAL_BUTTON_STATE_RELEASED;
  assert(h2_pal_button_read_single_button(api, 7u, &reading) == H2_PAL_OK);
  assert(reading.state == H2_PAL_BUTTON_STATE_RELEASED);
  assert(reader.calls == 2u);

  reader.result = H2_PAL_ERR_IO;
  assert(h2_pal_button_read_single_button(api, 7u, &reading) == H2_PAL_ERR_IO);
  assert(reader.calls == 3u);

  assert(h2_pal_button_read_single_button(api, 8u, &reading) ==
         H2_PAL_ERR_NOT_FOUND);
  h2_pal_radio_button_group_reading_t group = {0};
  assert(h2_pal_button_read_radio_button_group(api, 7u, &group) ==
         H2_PAL_ERR_UNSUPPORTED);
}

static void test_invalid_configuration(void) {
  const h2_linux_evdev_button_config_t duplicate[] = {
      {.periph_id = 1u, .device_name = "first", .key_code = 1u},
      {.periph_id = 1u, .device_name = "second", .key_code = 2u},
  };
  const h2_linux_evdev_button_config_t invalid_id = {
      .periph_id = 0u,
      .device_name = "button",
      .key_code = 1u,
  };
  assert(h2_linux_configure_evdev_buttons(NULL, 1u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_linux_configure_evdev_buttons(&invalid_id, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_linux_configure_evdev_buttons(duplicate, 2u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_linux_configure_evdev_buttons(NULL, 0u) == H2_PAL_OK);
}

int main(void) {
  test_configuration_and_reads();
  test_invalid_configuration();
  h2_linux_evdev_test_set_reader(NULL, NULL);
  puts("linux evdev button tests passed");
  return 0;
}
