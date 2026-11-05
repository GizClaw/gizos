/* Exercise the actual board power transition with deterministic SDK responses.
 */
#include "../ap/src/h2_bk7258_board_power.c"
static void reset_test(void) {
  key_pressed = sleeps = config_calls = fail_config = request_calls = entered =
      0;
  audio_error = audio_calls = radio_calls = reset_reason = wake_id =
      gpio_cleanup = 0;
  reboots = 0;
  native_power = (native_power_test_t){.selected = 1u};
  s_power_state = H2_PAL_POWER_STATE_RUNNING;
}

typedef struct partition_visit {
  uint32_t count;
  int stop;
} partition_visit_t;

static h2_pal_result_t visit_partition(
    void* user, const h2_pal_power_boot_partition_t* partition) {
  partition_visit_t* visit = user;
  assert(partition->id == ++visit->count);
  return visit->stop ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
}

static void test_native_boot_operations(const h2_pal_power_api_t* api) {
  reset_test();
  h2_pal_power_capabilities_t capabilities;
  assert(h2_pal_power_get_capabilities(api, &capabilities) == H2_PAL_OK);
  const uint32_t board_flags = H2_PAL_POWER_CAPABILITY_DEEP_SLEEP |
                               H2_PAL_POWER_CAPABILITY_BOOT_SOURCE |
                               H2_PAL_POWER_CAPABILITY_RESET_REASON;
  assert(capabilities.flags == (TEST_NATIVE_POWER_FLAGS | board_flags));
  partition_visit_t visit = {0};
  assert(h2_pal_power_list_boot_partitions(api, visit_partition, &visit) ==
         H2_PAL_OK);
  assert(visit.count == 2u);
  visit = (partition_visit_t){.stop = 1};
  assert(h2_pal_power_list_boot_partitions(api, visit_partition, &visit) ==
         H2_PAL_ERR_CLOSED);
  assert(visit.count == 1u);

  h2_pal_power_boot_partition_t partition;
  assert(h2_pal_power_get_running_boot_partition(api, &partition) == H2_PAL_OK);
  assert(partition.id == 1u);
  const h2_pal_power_boot_request_t request = {
      .partition_id = 2u,
      .policy = H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY};
  h2_pal_power_native_evidence_t evidence;
  assert(h2_pal_power_prepare_next_boot_partition(api, &request, &evidence) ==
         H2_PAL_OK);
  assert(native_power.prepared == 2u &&
         native_power.policy == H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY);
  assert(h2_pal_power_get_next_boot_partition(api, &partition) == H2_PAL_OK);
  assert(partition.id == 1u);
  assert(h2_pal_power_set_next_boot_partition(api, 2u) == H2_PAL_OK);
  assert(h2_pal_power_get_next_boot_partition(api, &partition) == H2_PAL_OK);
  assert(partition.id == 2u);

  h2_pal_power_image_state_t image;
  assert(h2_pal_power_get_image_state(api, 2u, &image) == H2_PAL_OK);
  assert(image.state == H2_PAL_POWER_IMAGE_STATE_PENDING_VERIFY &&
         image.flags == H2_PAL_POWER_IMAGE_FLAG_CONFIRMATION_REQUIRED);
  h2_pal_power_image_confirmation_t confirmation;
  assert(h2_pal_power_confirm_running_image(api, &evidence, &confirmation) ==
         H2_PAL_OK);
  assert(confirmation.partition_id == 1u && confirmation.was_pending == 1u &&
         confirmation.flags == H2_PAL_POWER_IMAGE_FLAG_RECOVERY_READY);
  assert(native_power.confirmations == 1u);
  assert(h2_pal_power_feed_watchdog(api) == H2_PAL_OK);
  assert(native_power.feeds == 1u);

  native_power.result = H2_PAL_ERR_BUSY;
  assert(h2_pal_power_get_capabilities(api, &capabilities) == H2_PAL_ERR_BUSY);
  assert(h2_pal_power_list_boot_partitions(api, visit_partition, &visit) ==
         H2_PAL_ERR_BUSY);
  assert(h2_pal_power_get_running_boot_partition(api, &partition) ==
         H2_PAL_ERR_BUSY);
  assert(h2_pal_power_get_next_boot_partition(api, &partition) ==
         H2_PAL_ERR_BUSY);
  assert(h2_pal_power_set_next_boot_partition(api, 1u) == H2_PAL_ERR_BUSY);
  assert(native_power.selected == 2u);
  assert(h2_pal_power_prepare_next_boot_partition(api, &request, &evidence) ==
         H2_PAL_ERR_BUSY);
  assert(evidence.format == 0u && evidence.size == 0u);
  assert(h2_pal_power_get_image_state(api, 2u, &image) == H2_PAL_ERR_BUSY);
  assert(image.state == H2_PAL_POWER_IMAGE_STATE_UNKNOWN && image.flags == 0u);
  assert(h2_pal_power_confirm_running_image(api, &evidence, &confirmation) ==
         H2_PAL_ERR_BUSY);
  assert(confirmation.partition_id == 0u && native_power.confirmations == 1u);
  assert(h2_pal_power_feed_watchdog(api) == H2_PAL_ERR_BUSY);

  h2_pal_power_hold_state_t hold;
  assert(h2_pal_power_set_hold(api, 1) == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_pal_power_get_hold(api, &hold) == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_pal_power_shutdown(api, 0u) == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_pal_power_sleep(api, 0u) == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_pal_power_set_deep_sleep_wake_timer(api, 1u) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(h2_pal_power_reboot(api, 23u) == H2_PAL_OK && reboots == 1);
  h2_pal_power_state_t state;
  assert(h2_pal_power_get_state(api, &state) == H2_PAL_OK &&
         state == H2_PAL_POWER_STATE_REBOOTING);
  assert(h2_bk7258_board_power_api() == api && critical_depth == 0u);
}

int main(void) {
  const h2_pal_power_api_t* api = h2_bk7258_board_power_api();
  test_native_boot_operations(api);
  reset_test();
  key_pressed = 1;
  assert(h2_pal_power_deep_sleep(api, 0) == H2_PAL_ERR_TIMEOUT);
  assert(sleeps == 5000 && config_calls == 0 && audio_calls == 0);
  for (int i = 1; i <= 3; i++) {
    reset_test();
    fail_config = i;
    assert(h2_pal_power_deep_sleep(api, 0) == H2_PAL_ERR_IO);
    assert(request_calls == 0 && gpio_cleanup == 2);
  }
  reset_test();
  audio_error = H2_PAL_ERR_BUSY;
  assert(h2_pal_power_deep_sleep(api, 0) == H2_PAL_ERR_BUSY &&
         config_calls == 0);
  reset_test();
  assert(h2_pal_power_deep_sleep(api, 0) == H2_PAL_ERR_TIMEOUT);
  assert(request_calls == 1 && radio_calls == 3 && gpio_cleanup == 3);
  assert(s_power_state == H2_PAL_POWER_STATE_RUNNING);
  reset_test();
  entered = 1;
  if (setjmp(asleep) == 0) {
    (void)h2_pal_power_deep_sleep(api, 0);
    assert(0);
  }
  assert(request_calls == 1 && gpio_cleanup == 0);
  assert(s_power_state == H2_PAL_POWER_STATE_DEEP_SLEEPING);
  h2_pal_power_boot_info_t boot;
  reset_reason = RESET_SOURCE_DEEPPS_GPIO;
  wake_id = 2;
  assert(h2_pal_power_get_boot_info(api, &boot) == H2_PAL_OK);
  assert(boot.source == H2_PAL_POWER_BOOT_SOURCE_GPIO_IRQ &&
         boot.source_id == 2);
  assert(boot.previous_transition ==
         H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP);
  puts("power host schedules PASS");
}
