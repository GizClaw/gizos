#include "h2_mfg.h"

static const h2_mfg_step_t steps[] = {
    {"storage", H2_MFG_STORAGE, 1},
    {"modem", H2_MFG_MODEM, 1},
    {"wifi", H2_MFG_WIFI, 1},
    {"ble", H2_MFG_BLE, 1},
};

const h2_mfg_profile_t h2_mfg_board_profile = {
    .board = "waveshare_esp32s3_a7670e_4g",
    .revision = 1,
    .steps = steps,
    .total = sizeof(steps) / sizeof(steps[0]),
    .screen_x_axis = 1,
    .screen_y_axis = 2,
};
