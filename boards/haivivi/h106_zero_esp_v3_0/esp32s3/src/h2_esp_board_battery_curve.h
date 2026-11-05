#ifndef H2_ESP_BOARD_BATTERY_CURVE_H
#define H2_ESP_BOARD_BATTERY_CURVE_H

#include <stdbool.h>
#include <stdint.h>

/** H106 product floor shared by percentage scaling and the low flag. */
#define H2_ESP_ZERO_BATTERY_USABLE_MIN_MV 3450
#define H2_ESP_ZERO_BATTERY_DISCHARGING_FULL_MV 4100
#define H2_ESP_ZERO_BATTERY_CHARGING_FULL_MV 4200

int32_t h2_esp_zero_esp_battery_voltage_mv_from_raw(int raw);
uint16_t h2_esp_zero_esp_battery_percent_x100(int32_t voltage_mv,
                                              bool charging);

#endif
