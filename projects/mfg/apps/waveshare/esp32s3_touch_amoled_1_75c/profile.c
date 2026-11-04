#include "h2_mfg.h"

static const h2_mfg_step_t steps[] = {
    {"storage", H2_MFG_STORAGE, 1}, {"display", H2_MFG_DISPLAY, 0},
    {"touch", H2_MFG_TOUCH, 0},     {"buttons", H2_MFG_BUTTONS, 0},
    {"imu", H2_MFG_IMU, 0},         {"speaker", H2_MFG_SPEAKER, 0},
    {"mic", H2_MFG_MIC, 0},         {"wifi", H2_MFG_WIFI, 1},
    {"ble", H2_MFG_BLE, 1},         {"aec", H2_MFG_AEC, 0},
};

const h2_mfg_profile_t h2_mfg_board_profile = {
    .board = "waveshare_esp32s3_touch_amoled_1_75c",
    .revision = 2,
    .steps = steps,
    .total = sizeof(steps) / sizeof(steps[0]),
    .append_from_total = 9,
    .append_from_revision = 1,
    .speaker_test_volume = 100,
    .buttons = {101, 102},
    .button_names = {"BOOT", "PWR"},
    .button_count = 2,
    .imu = 301,
    /* Mounting verified by the operator on 2026-10-29. Raw PAL axes stay
       native. */
    .screen_x_axis = 2,
    .screen_y_axis = 1,
};
