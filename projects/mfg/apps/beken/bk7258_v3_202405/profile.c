#include "h2_mfg.h"

static const h2_mfg_step_t steps[] = {
    {"storage", H2_MFG_STORAGE, 1}, {"display", H2_MFG_DISPLAY, 0},
    {"touch", H2_MFG_TOUCH, 0},     {"buttons", H2_MFG_BUTTONS, 0},
    {"speaker", H2_MFG_SPEAKER, 0}, {"mic", H2_MFG_MIC, 0},
    {"wifi", H2_MFG_WIFI, 1},       {"ble", H2_MFG_BLE, 1},
    {"aec", H2_MFG_AEC, 0},
};

const h2_mfg_profile_t h2_mfg_board_profile = {
    .board = "bk7258_v3_202405",
    .revision = 2,
    .steps = steps,
    .total = sizeof(steps) / sizeof(steps[0]),
    .append_from_total = 8,
    .append_from_revision = 1,
    .buttons = {101, 102, 104},
    .button_names = {"KEY1", "KEY2", "KEY3"},
    .button_count = 3,
    .button_group = 100,
};
