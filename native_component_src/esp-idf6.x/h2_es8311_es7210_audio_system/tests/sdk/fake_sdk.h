#ifndef H2_AUDIO_FAKE_SDK_H
#define H2_AUDIO_FAKE_SDK_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_TIMEOUT 4
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { int err_ = (expr); if (err_ != ESP_OK) return err_; } while (0)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTRUE 1
#define pdPASS 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
void vTaskDeleteWithCaps(TaskHandle_t task);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *, BaseType_t);
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *, BaseType_t, uint32_t);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
typedef int gpio_num_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
int gpio_config(const gpio_config_t *);
int gpio_set_level(gpio_num_t, unsigned);
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt; struct { bool enable_internal_pullup; } flags; } i2c_master_bus_config_t;
typedef struct { int dev_addr_length; uint16_t device_address; uint32_t scl_speed_hz; } i2c_device_config_t;
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0
int i2c_new_master_bus(const i2c_master_bus_config_t *, i2c_master_bus_handle_t *);
int i2c_master_get_bus_handle(int, i2c_master_bus_handle_t *);
int i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t *, i2c_master_dev_handle_t *);
int i2c_master_bus_rm_device(i2c_master_dev_handle_t);
int i2c_del_master_bus(i2c_master_bus_handle_t);
int i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t *, size_t, int);
int i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t *, size_t, uint8_t *, size_t, int);
typedef void *i2s_chan_handle_t;
typedef struct { bool auto_clear; } i2s_chan_config_t;
typedef struct { struct { unsigned mclk_multiple; } clk_cfg; int slot_cfg; struct { int mclk, bclk, ws, dout, din; struct { int unused; } invert_flags; } gpio_cfg; } i2s_std_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) ((i2s_chan_config_t){0})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) {0}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) 0
int i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *, i2s_chan_handle_t *);
int i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t *);
int i2s_channel_enable(i2s_chan_handle_t);
int i2s_channel_disable(i2s_chan_handle_t);
int i2s_del_channel(i2s_chan_handle_t);
int i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *, uint32_t);
int i2s_channel_write(i2s_chan_handle_t, const void *, size_t, size_t *, uint32_t);
#endif
