#ifndef H2_P4_TEST_TEMPERATURE_H
#define H2_P4_TEST_TEMPERATURE_H
#include "esp_err.h"
typedef void* temperature_sensor_handle_t;
typedef struct {
  int low, high;
} temperature_sensor_config_t;
#define TEMPERATURE_SENSOR_CONFIG_DEFAULT(lo, hi) {.low = (lo), .high = (hi)}
esp_err_t temperature_sensor_install(const temperature_sensor_config_t* config,
                                     temperature_sensor_handle_t* out);
esp_err_t temperature_sensor_enable(temperature_sensor_handle_t handle);
esp_err_t temperature_sensor_uninstall(temperature_sensor_handle_t handle);
esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t handle,
                                         float* out);
#endif
