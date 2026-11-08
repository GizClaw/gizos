#ifndef H2_BK7258_BOARD_TOUCH_TEST_SDK_H
#define H2_BK7258_BOARD_TOUCH_TEST_SDK_H

#include <stdbool.h>
#include <stdint.h>

#define CONFIG_TP 1
#define CONFIG_SIM_I2C_HW_BOARD_V3 1
#define CONFIG_TP_INT_GPIO_ID 6
#define CONFIG_TP_RST_GPIO_ID 9
#define CONFIG_TP_HY4633 0
#define CONFIG_TP_FT6336 0
#define TP_SUPPORT_MAX_NUM 1
#define BK_OK 0
enum { TP_ID_UNKNOW, TP_ID_GT911, TP_ID_GT1151 };

typedef int gpio_id_t;
typedef int gpio_int_type_t;
typedef void (*gpio_isr_t)(gpio_id_t id);
typedef struct {
  int io_mode;
  int pull_mode;
} gpio_config_t;
enum { GPIO_INPUT_ENABLE, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE };
enum { GPIO_INT_TYPE_RISING_EDGE, GPIO_INT_TYPE_FALLING_EDGE };
typedef struct {
  int baud_rate;
  int addr_mode;
} i2c_config_t;
enum { I2C_BAUD_RATE_100KHZ, I2C_ADDR_MODE_7BIT };
typedef enum {
  TP_INT_TYPE_RISING_EDGE,
  TP_INT_TYPE_FALLING_EDGE,
  TP_INT_TYPE_LOW_LEVEL,
  TP_INT_TYPE_HIGH_LEVEL
} tp_int_type_t;
enum {
  TP_EVENT_TYPE_NONE,
  TP_EVENT_TYPE_DOWN,
  TP_EVENT_TYPE_UP,
  TP_EVENT_TYPE_MOVE
};
typedef struct {
  uint8_t event, track_id;
  uint16_t x_coordinate, y_coordinate;
  uint8_t width;
  uint32_t timestamp;
} tp_data_t;
typedef struct {
  int (*read_uint8)(uint8_t, uint8_t, uint8_t*, uint16_t);
  int (*write_uint8)(uint8_t, uint8_t, uint8_t*, uint16_t);
  int (*read_uint16)(uint8_t, uint16_t, uint8_t*, uint16_t);
  int (*write_uint16)(uint8_t, uint16_t, uint8_t*, uint16_t);
} tp_i2c_callback_t;
typedef struct {
  uint16_t x_size, y_size;
  tp_int_type_t int_type;
  uint8_t refresh_rate, tp_num;
} tp_sensor_user_config_t;
typedef struct {
  char* name;
  uint8_t id;
  int def_ppi;
  tp_int_type_t def_int_type;
  uint8_t def_refresh_rate, def_tp_num, address;
  bool (*detect)(const tp_i2c_callback_t*);
  int (*init)(const tp_i2c_callback_t*, tp_sensor_user_config_t*);
  int (*read_tp_info)(const tp_i2c_callback_t*, uint8_t, uint8_t*);
} tp_sensor_config_t;
uint32_t rtos_enter_critical(void);
void rtos_exit_critical(uint32_t flags);
int rtos_delay_milliseconds(uint32_t ms);
int gpio_dev_unmap(gpio_id_t id);
int bk_gpio_set_config(gpio_id_t id, const gpio_config_t* config);
int bk_gpio_set_output_high(gpio_id_t id);
int bk_gpio_set_output_low(gpio_id_t id);
int bk_gpio_disable_interrupt(gpio_id_t id);
int bk_gpio_enable_interrupt(gpio_id_t id);
int bk_gpio_clear_interrupt(gpio_id_t id);
int bk_gpio_set_interrupt_type(gpio_id_t id, gpio_int_type_t type);
int bk_gpio_register_isr(gpio_id_t id, gpio_isr_t isr);
int bk_i2c_init_v2(int id, const i2c_config_t* config);
int bk_i2c_deinit_v2(int id);
int tp_i2c_read_uint8(uint8_t, uint8_t, uint8_t*, uint16_t);
int tp_i2c_write_uint8(uint8_t, uint8_t, uint8_t*, uint16_t);
int tp_i2c_read_uint16(uint8_t, uint16_t, uint8_t*, uint16_t);
int tp_i2c_write_uint16(uint8_t, uint16_t, uint8_t*, uint16_t);
void tp_sensor_devices_init(void);
#endif
