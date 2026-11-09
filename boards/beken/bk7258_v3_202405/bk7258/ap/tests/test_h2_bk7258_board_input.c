#include "h2_bk7258_board_private.h"
#include "h2_bk7258_board_input_test_sdk.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

typedef enum {
  ACQUIRE,
  UNMAP,
  INIT,
  GPIO_MAP,
  CONFIG,
  BYPASS,
  START,
  CHANNEL,
  READ,
  DELAY,
  STOP,
  DEINIT,
  RELEASE,
  OP_COUNT
} operation_t;

static struct {
  bool owned, initialized, mapped, configured, bypass, started;
  unsigned calls[OP_COUNT];
  int fail;
  unsigned fail_after;
  uint16_t raw;
  adc_chan_t channel;
  unsigned reads;
} sdk = {.fail = -1};

static int operation(operation_t op) {
  sdk.calls[op]++;
  if (sdk.fail == (int)op) {
    if (sdk.fail_after > 0) {
      --sdk.fail_after;
    } else {
      sdk.fail = -1;
      return BK_FAIL;
    }
  }
  return BK_OK;
}

bk_err_t gpio_dev_unmap(gpio_id_t gpio) {
  assert(sdk.owned && gpio == GPIO_12);
  int rc = operation(UNMAP);
  if (rc == BK_OK) sdk.mapped = false;
  return rc;
}
bk_err_t gpio_dev_map(gpio_id_t gpio, gpio_dev_t dev) {
  assert(sdk.owned && gpio == GPIO_12 && dev == GPIO_DEV_ADC14);
  int rc = operation(GPIO_MAP);
  if (rc == BK_OK) sdk.mapped = true;
  return rc;
}
bk_err_t rtos_delay_milliseconds(uint32_t ms) {
  assert(sdk.owned && ms == 5);
  return operation(DELAY);
}
bk_err_t bk_adc_acquire(void) {
  int rc = operation(ACQUIRE);
  if (rc == BK_OK) {
    assert(!sdk.owned);
    sdk.owned = true;
  }
  return rc;
}
bk_err_t bk_adc_init(adc_chan_t chan) {
  assert(sdk.owned && chan == 14);
  int rc = operation(INIT);
  if (rc == BK_OK) {
    sdk.initialized = true;
    sdk.reads = 0;
  }
  return rc;
}
bk_err_t bk_adc_chan_init_gpio(adc_chan_t chan) {
  assert(sdk.owned && chan == 14);
  /* The CP table omits this AP-owned pad. Its helper ignores a map failure. */
  return BK_OK;
}
bk_err_t bk_adc_set_config(adc_config_t* config) {
  assert(sdk.owned && sdk.initialized);
  assert(config->chan == 14 && config->adc_mode == ADC_CONTINUOUS_MODE);
  assert(config->src_clk == ADC_SCLK_XTAL_26M && config->clk == 3203125);
  assert(config->saturate_mode == ADC_SATURATE_MODE_3);
  assert(config->steady_ctrl == 7 && config->adc_filter == 0);
  int rc = operation(CONFIG);
  if (rc == BK_OK) sdk.configured = true;
  return rc;
}
bk_err_t bk_adc_enable_bypass_clalibration(void) {
  assert(sdk.owned && sdk.configured);
  int rc = operation(BYPASS);
  if (rc == BK_OK) sdk.bypass = true;
  return rc;
}
bk_err_t bk_adc_start(void) {
  assert(sdk.owned && sdk.initialized && sdk.configured && sdk.bypass);
  int rc = operation(START);
  if (sdk.started) return BK_FAIL;
  if (rc == BK_OK) sdk.started = true;
  return rc;
}
bk_err_t bk_adc_set_channel(adc_chan_t chan) {
  assert(sdk.owned && chan == 14);
  int rc = operation(CHANNEL);
  if (rc == BK_OK) sdk.channel = chan;
  return rc;
}
bk_err_t bk_adc_read(uint16_t* raw, uint32_t timeout_ms) {
  assert(sdk.owned && sdk.started && timeout_ms > 0);
  int rc = operation(READ);
  if (rc == BK_OK) {
    /* A simulated priming conversion must never become a button press. */
    *raw = sdk.reads++ == 0 ? 46u : (sdk.mapped ? sdk.raw : 46u);
  }
  return rc;
}
bk_err_t bk_adc_stop(void) {
  assert(sdk.owned);
  /* SDK stop drains samples and masks the IRQ; deinit powers down ADC. */
  return operation(STOP);
}
bk_err_t bk_adc_deinit(adc_chan_t chan) {
  assert(sdk.owned && chan == 14);
  int rc = operation(DEINIT);
  if (rc == BK_OK)
    sdk.initialized = sdk.configured = sdk.bypass = sdk.started = false;
  return rc;
}
bk_err_t bk_adc_release(void) {
  assert(sdk.owned);
  int rc = operation(RELEASE);
  if (rc == BK_OK) {
    sdk.owned = false;
    /* Another user may replace the shared ADC configuration and ISR. */
    sdk.initialized = sdk.configured = sdk.bypass = sdk.started = false;
  }
  return rc;
}
bk_err_t bk_phy_get_current_temperature(float* value) {
  *value = 23.5f;
  return BK_OK;
}

static h2_pal_result_t read_group(
    h2_pal_radio_button_group_reading_t* reading) {
  return h2_pal_button_read_radio_button_group(
      h2_bk7258_board_button_api(), H2_BK7258_ADC_BUTTON_GROUP_ID, reading);
}
static void expect_raw(uint16_t raw, h2_pal_periph_id_t id) {
  sdk.raw = raw;
  h2_pal_radio_button_group_reading_t reading = {0};
  assert(read_group(&reading) == H2_PAL_OK);
  assert(reading.id == H2_BK7258_ADC_BUTTON_GROUP_ID);
  assert(reading.pressed_button_id == id);
  assert(!sdk.owned && !sdk.started && !sdk.initialized);
  assert(sdk.channel == 14 && sdk.reads == 2);
}
static void expect_failed_read(void) {
  h2_pal_radio_button_group_reading_t reading = {.id = 777,
                                                 .pressed_button_id = 888};
  assert(read_group(&reading) != H2_PAL_OK);
  assert(reading.id == 777 && reading.pressed_button_id == 888);
  expect_raw(7822, 0);
}
static void test_transactions_and_failures(void) {
  expect_raw(1517, 101);
  expect_raw(7822, 0);
  assert(sdk.calls[INIT] == 2 && sdk.calls[CONFIG] == 2);
  assert(sdk.calls[GPIO_MAP] == 2 && sdk.calls[DEINIT] == 2);
  assert(sdk.calls[ACQUIRE] == sdk.calls[RELEASE]);
  for (int op = 0; op < OP_COUNT; ++op) {
    sdk.fail = op;
    sdk.fail_after = 0;
    expect_failed_read();
  }
  sdk.fail = READ;
  sdk.fail_after = 1;
  expect_failed_read();
}
static h2_pal_result_t count_periph(void* user,
                                    const h2_pal_periph_info_t* info) {
  size_t* count = user;
  assert(info->type == H2_PAL_PERIPH_TYPE_RADIO_BUTTON);
  assert(info->id >= 101 && info->id <= 104);
  assert(info->payload_size == sizeof(h2_pal_periph_radio_button_payload_t));
  const h2_pal_periph_radio_button_payload_t* payload = info->payload;
  assert(payload->group_id == H2_BK7258_ADC_BUTTON_GROUP_ID);
  *count += 1;
  return H2_PAL_OK;
}
static void test_four_keys_and_boundaries(void) {
  /* Observed physical press/release limits on the version-18 DevKit run. */
  expect_raw(43, 102);
  expect_raw(44, 102);
  expect_raw(1514, 101);
  expect_raw(1517, 101);
  expect_raw(2970, 103);
  expect_raw(2976, 103);
  expect_raw(4539, 104);
  expect_raw(4547, 104);
  expect_raw(7811, 0);
  expect_raw(7833, 0);
  /* Retain the original measured lower levels and inferred upper centers. */
  expect_raw(0, 102);
  expect_raw(49, 102);
  expect_raw(1517, 101);
  expect_raw(2985, 103);
  expect_raw(4453, 104);
  /* The old KEY4 voltage band swallowed the reported physical KEY3. */
  expect_raw(2560, 103);
  expect_raw(3329, 103);
  expect_raw(6000, 104);
  expect_raw(7000, 0);
  expect_raw(7822, 0);
  expect_raw(8191, 0);
  expect_raw(UINT16_MAX, 0);
  assert(h2_bk7258_button_range_count == 4);
  bool seen[4] = {false};
  for (size_t i = 0; i < h2_bk7258_button_range_count; ++i) {
    const h2_bk7258_button_adc_range_t* r = &h2_bk7258_button_ranges[i];
    assert(r->id >= 101 && r->id <= 104 && !seen[r->id - 101]);
    seen[r->id - 101] = true;
    expect_raw(r->min_raw, r->id);
    expect_raw((uint16_t)((r->min_raw + r->max_raw) / 2u), r->id);
    expect_raw(r->max_raw, r->id);
    if (i > 0)
      expect_raw((uint16_t)(r->min_raw - 1u),
                 h2_bk7258_button_ranges[i - 1u].id);
    expect_raw((uint16_t)(r->max_raw + 1u),
               i + 1u < h2_bk7258_button_range_count
                   ? h2_bk7258_button_ranges[i + 1u].id
                   : 0u);
  }
  size_t count = 0;
  const h2_pal_periph_api_t* api = h2_bk7258_board_periph_api();
  assert(api->vtable->list(api->user, H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
                           count_periph, &count) == H2_PAL_OK &&
         count == 4);
  h2_pal_periph_info_t info;
  assert(api->vtable->get(api->user, 103, &info) == H2_PAL_OK);
  assert(info.type == H2_PAL_PERIPH_TYPE_RADIO_BUTTON);
}
static void test_invalid_ids_do_not_sample(void) {
  unsigned before = sdk.calls[ACQUIRE];
  const h2_pal_button_api_t* api = h2_bk7258_board_button_api();
  h2_pal_radio_button_group_reading_t group;
  h2_pal_single_button_reading_t single;
  assert(h2_pal_button_read_radio_button_group(api, 103, &group) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_button_read_radio_button_group(api, 999, &group) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(h2_pal_button_read_radio_button_group(api, 100, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_button_read_single_button(api, 103, &single) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(before == sdk.calls[ACQUIRE]);
}
int main(void) {
  test_transactions_and_failures();
  test_four_keys_and_boundaries();
  test_invalid_ids_do_not_sample();
  puts("BK7258 P12/ADC14 four-key sampling and failure cleanup passed");
  return 0;
}
