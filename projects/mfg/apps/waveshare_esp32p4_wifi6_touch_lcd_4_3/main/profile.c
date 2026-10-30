#include "h2_mfg.h"

static const h2_mfg_step_t steps[] = {
    {"storage", H2_MFG_STORAGE, 1}, {"display", H2_MFG_DISPLAY, 0},
    {"touch", H2_MFG_TOUCH, 0},     {"buttons", H2_MFG_BUTTONS, 0},
    {"speaker", H2_MFG_SPEAKER, 0}, {"mic", H2_MFG_MIC, 0},
    {"wifi", H2_MFG_WIFI, 1},       {"ble", H2_MFG_BLE, 1},
    {"aec", H2_MFG_AEC, 0},         {"camera", H2_MFG_CAMERA, 0},
};

const h2_mfg_profile_t h2_mfg_board_profile = {
    .board = "waveshare_esp32p4_wifi6_touch_lcd_4_3",
    .revision = 3,
    .steps = steps,
    .total = sizeof(steps) / sizeof(steps[0]),
    .append_from_total = 9,
    .append_from_revision = 2,
    .speaker_test_volume = 100,
    .buttons = {101},
    .button_names = {"BOOT"},
    .button_count = 1,
    .has_reset_button = 1,
    .has_power_button = 1,
    .screen_x_axis = 1,
    .screen_y_axis = 2,
};
