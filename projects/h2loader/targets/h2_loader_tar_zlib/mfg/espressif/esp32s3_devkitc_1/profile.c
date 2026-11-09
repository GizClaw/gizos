#include "h2_mfg.h"

static const h2_mfg_step_t steps[] = {
    {"storage", H2_MFG_STORAGE, 1}, {"buttons", H2_MFG_BUTTONS, 0},
    {"led", H2_MFG_LED, 0},         {"wifi", H2_MFG_WIFI, 1},
    {"ble", H2_MFG_BLE, 1},
};

const h2_mfg_profile_t h2_mfg_board_profile = {
    .board = "devkit",
    .revision = 1,
    .steps = steps,
    .total = sizeof(steps) / sizeof(steps[0]),
    .buttons = {101},
    .button_names = {"BOOT"},
    .button_count = 1,
    .screen_x_axis = 1,
    .screen_y_axis = 2,
    .led = 601,
};
