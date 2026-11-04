#ifndef H2_TEST_RUNTIME_BSP_H
#define H2_TEST_RUNTIME_BSP_H
typedef enum {
  BSP_BOARD_VARIANT_V1_0,
  BSP_BOARD_VARIANT_V1_2
} bsp_board_variant_t;
int bsp_board_variant_get(bsp_board_variant_t* variant);
#define BSP_I2C_SDA_V1_0 0
#define BSP_I2C_SCL_V1_0 1
#define BSP_I2C_SDA_V1_2 56
#define BSP_I2C_SCL_V1_2 3
#define ESP_OK 0
int bsp_power_init(void);
int bsp_i2c_init(void);
#endif
