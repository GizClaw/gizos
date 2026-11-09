#include "h2_zero_bk_board_config.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* SDK-independent view of the board-owned GPIO table. CP sleep preserves
 * output only for entries with a KEEP policy; INIT controls boot-time writes.
 */
enum {
  GPIO_10 = 10,
  GPIO_11 = 11,
  GPIO_13 = 13,
  GPIO_SECOND_FUNC_DISABLE,
  GPIO_SECOND_FUNC_ENABLE,
  GPIO_DEV_INVALID,
  GPIO_DEV_UART0_RXD,
  GPIO_DEV_UART0_TXD,
  GPIO_IO_DISABLE,
  GPIO_PULL_DISABLE,
  GPIO_PULL_UP_EN,
  GPIO_INT_DISABLE,
  GPIO_INT_TYPE_LOW_LEVEL,
  GPIO_LOW_POWER_DISCARD_IO_STATUS,
  GPIO_LOW_POWER_KEEP_INPUT_STATUS,
  GPIO_LOW_POWER_KEEP_OUTPUT_STATUS,
  GPIO_DRIVER_CAPACITY_0,
  GPIO_INIT_DISABLE,
  GPIO_INIT_ENABLE,
  GPIO_TIME_SHARING_MULTIPLEX_DISABLE,
};
typedef struct {
  unsigned gpio, function, device, io, pull, interrupt, interrupt_type;
  unsigned low_power, capacity, initialize, multiplex;
} gpio_policy_t;

#include "../cp/usr_gpio_cfg.h"

int main(void) {
  static const gpio_policy_t policies[] = GPIO_DEFAULT_DEV_CONFIG;
  unsigned rail_entries = 0u;
  uint64_t kept_outputs = 0u;
  for (size_t i = 0; i < sizeof(policies) / sizeof(policies[0]); ++i) {
    const gpio_policy_t* policy = &policies[i];
    if (policy->low_power == GPIO_LOW_POWER_KEEP_OUTPUT_STATUS)
      kept_outputs |= UINT64_C(1) << policy->gpio;
    if (policy->gpio == H2_ZERO_BK_PERIPHERAL_ENABLE_GPIO) {
      ++rail_entries;
      assert(policy->low_power == GPIO_LOW_POWER_KEEP_OUTPUT_STATUS);
      assert(policy->initialize == GPIO_INIT_DISABLE);
      assert(policy->function == GPIO_SECOND_FUNC_DISABLE);
    }
  }
  assert(rail_entries == 1u);
  assert((kept_outputs & (UINT64_C(1) << H2_ZERO_BK_PERIPHERAL_ENABLE_GPIO)) !=
         0u);
  puts("CP retains the AP-owned shared Flash supply PASS");
  return 0;
}
