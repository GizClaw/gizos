#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define GPIO_DEFAULT_DEV_CONFIG                                                \
  {                                                                            \
      {GPIO_5, GPIO_SECOND_FUNC_ENABLE, GPIO_DEV_INVALID, GPIO_INPUT_ENABLE,   \
       GPIO_PULL_UP_EN, GPIO_INT_ENABLE, GPIO_INT_TYPE_FALLING_EDGE,           \
       GPIO_LOW_POWER_KEEP_INPUT_STATUS, GPIO_DRIVER_CAPACITY_0,               \
       GPIO_INIT_ENABLE, GPIO_TIME_SHARING_MULTIPLEX_ENABLE},                  \
      {GPIO_10, GPIO_SECOND_FUNC_ENABLE, GPIO_DEV_UART0_RXD, GPIO_IO_DISABLE,  \
       GPIO_PULL_UP_EN, GPIO_INT_DISABLE, GPIO_INT_TYPE_LOW_LEVEL,             \
       GPIO_LOW_POWER_DISCARD_IO_STATUS, GPIO_DRIVER_CAPACITY_0,               \
       GPIO_INIT_ENABLE, GPIO_TIME_SHARING_MULTIPLEX_ENABLE},                  \
      {GPIO_11, GPIO_SECOND_FUNC_ENABLE, GPIO_DEV_UART0_TXD, GPIO_IO_DISABLE,  \
       GPIO_PULL_UP_EN, GPIO_INT_DISABLE, GPIO_INT_TYPE_LOW_LEVEL,             \
       GPIO_LOW_POWER_DISCARD_IO_STATUS, GPIO_DRIVER_CAPACITY_0,               \
       GPIO_INIT_ENABLE, GPIO_TIME_SHARING_MULTIPLEX_ENABLE},                  \
  }

#ifdef __cplusplus
}
#endif
