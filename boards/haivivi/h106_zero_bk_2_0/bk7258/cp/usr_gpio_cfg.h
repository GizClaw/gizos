#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* AP initializes the shared Flash/peripheral rail. CP must retain its output
 * through low voltage without reinitializing the pin when CP starts. */
#define GPIO_DEFAULT_DEV_CONFIG                                               \
  {                                                                           \
      {GPIO_13, GPIO_SECOND_FUNC_DISABLE, GPIO_DEV_INVALID, GPIO_IO_DISABLE,  \
       GPIO_PULL_DISABLE, GPIO_INT_DISABLE, GPIO_INT_TYPE_LOW_LEVEL,          \
       GPIO_LOW_POWER_KEEP_OUTPUT_STATUS, GPIO_DRIVER_CAPACITY_0,             \
       GPIO_INIT_DISABLE, GPIO_TIME_SHARING_MULTIPLEX_DISABLE},               \
      {GPIO_10, GPIO_SECOND_FUNC_ENABLE, GPIO_DEV_UART0_RXD, GPIO_IO_DISABLE, \
       GPIO_PULL_UP_EN, GPIO_INT_DISABLE, GPIO_INT_TYPE_LOW_LEVEL,            \
       GPIO_LOW_POWER_DISCARD_IO_STATUS, GPIO_DRIVER_CAPACITY_0,              \
       GPIO_INIT_ENABLE, GPIO_TIME_SHARING_MULTIPLEX_DISABLE},                \
      {GPIO_11, GPIO_SECOND_FUNC_ENABLE, GPIO_DEV_UART0_TXD, GPIO_IO_DISABLE, \
       GPIO_PULL_UP_EN, GPIO_INT_DISABLE, GPIO_INT_TYPE_LOW_LEVEL,            \
       GPIO_LOW_POWER_DISCARD_IO_STATUS, GPIO_DRIVER_CAPACITY_0,              \
       GPIO_INIT_ENABLE, GPIO_TIME_SHARING_MULTIPLEX_DISABLE},                \
  }

#ifdef __cplusplus
}
#endif
