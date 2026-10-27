#ifndef H2_TEST_AUDIO_BSP_H
#define H2_TEST_AUDIO_BSP_H
#include <stdbool.h>
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
#define BSP_I2C_PORT 0
#define BSP_AUDIO_I2S_MCLK 54
#define BSP_AUDIO_I2S_SCLK 37
#define BSP_AUDIO_I2S_LRCLK 49
#define BSP_AUDIO_I2S_SDOUT 40
#define BSP_AUDIO_I2S_DSIN 52
#define BSP_AUDIO_PA_CTRL 45
#define tskNO_AFFINITY -1
int bsp_power_set_vcc_3v3(bool on);
int bsp_power_set_codec_3v3(bool on);
void* bsp_i2c_get_handle(void);
#endif
