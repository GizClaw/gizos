#include "h2_zero_bk_peripheral_power.h"
#include "h2_zero_bk_peripheral_power_test_sdk.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool output_enabled[48];
static bool output_high[48];
static bool rail_live;
static unsigned interruptions;
static unsigned unmaps_of_rail;
static unsigned operation_count;
static unsigned fail_operation;
static uint32_t critical_depth;
static const uint32_t preserved_fields = (2u << 8) | (1u << 10) | (1u << 7);

static void observe_supply(void) {
  const unsigned pin = H2_ZERO_BK_PERIPHERAL_ENABLE_GPIO;
  if (rail_live && (!output_enabled[pin] || !output_high[pin])) ++interruptions;
}

static bool operation_failed(gpio_id_t gpio) {
  assert(gpio >= 0 && gpio < 48);
  if (gpio == H2_ZERO_BK_PERIPHERAL_ENABLE_GPIO) assert(critical_depth > 0u);
  return ++operation_count == fail_operation;
}

uint32_t rtos_enter_critical(void) { return critical_depth++; }
void rtos_exit_critical(uint32_t previous) {
  assert(critical_depth == previous + 1u);
  critical_depth = previous;
}

int gpio_dev_unmap(gpio_id_t gpio) {
  if (operation_failed(gpio)) return -1;
  /* Match the SDK's relevant side effect, including an already-GPIO pin. */
  output_enabled[gpio] = false;
  if (gpio == H2_ZERO_BK_PERIPHERAL_ENABLE_GPIO) ++unmaps_of_rail;
  observe_supply();
  return BK_OK;
}

int bk_gpio_set_config(gpio_id_t gpio, const gpio_config_t* config) {
  if (operation_failed(gpio)) return -1;
  if (gpio == H2_ZERO_BK_PERIPHERAL_ENABLE_GPIO) {
    /* The first output-enable must also avoid driving a stale low latch. */
    assert(output_high[gpio]);
    assert(config->func_mode == GPIO_SECOND_FUNC_DISABLE);
  }
  output_enabled[gpio] = config->io_mode == GPIO_OUTPUT_ENABLE;
  observe_supply();
  return BK_OK;
}

int bk_gpio_set_output_high(gpio_id_t gpio) {
  if (operation_failed(gpio)) return -1;
  /* The pinned SDK rejects changing the latch on an output-disabled pin. */
  if (!output_enabled[gpio]) return -2;
  output_high[gpio] = true;
  observe_supply();
  return BK_OK;
}

int bk_gpio_set_output_low(gpio_id_t gpio) {
  if (operation_failed(gpio)) return -1;
  if (!output_enabled[gpio]) return -2;
  output_high[gpio] = false;
  observe_supply();
  return BK_OK;
}

uint32_t bk_gpio_get_value(gpio_id_t gpio) {
  assert(critical_depth > 0u);
  return preserved_fields | (output_high[gpio] ? 2u : 0u) |
         (output_enabled[gpio] ? 0u : 8u);
}

int bk_gpio_set_value(gpio_id_t gpio, uint32_t value) {
  if (operation_failed(gpio)) return -1;
  assert((value & ((3u << 8) | (3u << 10) | (1u << 7))) == preserved_fields);
  /* Observe the complete peripheral write, so a temporary low/disabled state
   * fails the same continuity check as the old unmap path. */
  output_high[gpio] = (value & 2u) != 0u;
  output_enabled[gpio] = (value & 8u) == 0u;
  observe_supply();
  return BK_OK;
}

static void reset_test(bool mounted) {
  memset(output_enabled, 0, sizeof(output_enabled));
  memset(output_high, 0, sizeof(output_high));
  interruptions = unmaps_of_rail = operation_count = fail_operation = 0u;
  critical_depth = 0u;
  rail_live = mounted;
  output_enabled[H2_ZERO_BK_PERIPHERAL_ENABLE_GPIO] = mounted;
  output_high[H2_ZERO_BK_PERIPHERAL_ENABLE_GPIO] = mounted;
}

int main(void) {
  reset_test(false);
  assert(h2_zero_bk_peripheral_power_enable() == H2_PAL_OK);
  rail_live = true;
  for (unsigned i = 0u; i < 100u; ++i) {
    /* Filesystem/NFC/modem acquisition must be harmless while audio cycles. */
    assert(h2_zero_bk_peripheral_power_enable() == H2_PAL_OK);
    assert(h2_bk7258_board_audio_power_set(0) == H2_PAL_OK);
    assert(!output_high[H2_ZERO_BK_AUDIO_PA_ENABLE_GPIO]);
    assert(h2_bk7258_board_audio_power_set(1) == H2_PAL_OK);
    assert(output_high[H2_ZERO_BK_AUDIO_PA_ENABLE_GPIO]);
    assert(!output_high[H2_ZERO_BK_AUDIO_SELECT_GPIO]);
  }
  assert(interruptions == 0u && unmaps_of_rail == 0u && critical_depth == 0u);

  reset_test(true);
  assert(h2_bk7258_board_audio_power_set(1) == H2_PAL_OK);
  const unsigned successful_operations = operation_count;
  for (unsigned operation = 1u; operation <= successful_operations;
       ++operation) {
    reset_test(true);
    fail_operation = operation;
    assert(h2_bk7258_board_audio_power_set(1) == H2_PAL_ERR_IO);
    assert(critical_depth == 0u && interruptions == 0u && unmaps_of_rail == 0u);
  }
  reset_test(false);
  fail_operation = 1u;
  assert(h2_zero_bk_peripheral_power_enable() == H2_PAL_ERR_IO);
  assert(operation_count == 1u);
  assert(!output_enabled[H2_ZERO_BK_PERIPHERAL_ENABLE_GPIO]);
  assert(critical_depth == 0u);

  reset_test(true);
  critical_depth = 2u;
  assert(h2_zero_bk_peripheral_power_enable() == H2_PAL_OK);
  assert(critical_depth == 2u && interruptions == 0u);
  puts("shared Flash rail continuity and error paths PASS");
  return 0;
}
