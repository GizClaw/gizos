#include "h2_mfg.h"

static const h2_mfg_step_t steps[] = {
    {"storage", H2_MFG_STORAGE, 1},
    {"display", H2_MFG_DISPLAY, 0},
    {"buttons", H2_MFG_BUTTONS, 0},
    {"speaker", H2_MFG_SPEAKER, 0},
    {"mic", H2_MFG_MIC, 0},
    {"wifi", H2_MFG_WIFI, 1},
    {"ble", H2_MFG_BLE, 1},
    {"led", H2_MFG_LED, 0},
    {"modem", H2_MFG_MODEM, 1},
    {"battery", H2_MFG_BATTERY, 1},
    {"vibration", H2_MFG_VIBRATION, 0},
    {"nfc", H2_MFG_NFC, 0},
    {"aec", H2_MFG_AEC, 0},
};

const h2_mfg_profile_t h2_mfg_board_profile = {
    .board = "tiga_esp_v4_2",
    .revision = 1,
    .steps = steps,
    .total = sizeof(steps) / sizeof(steps[0]),
    .buttons = {102, 103, 104, 105, 106, 107, 108, 101},
    .button_names = {"OK", "LEFT", "RIGHT", "BACK", "VOL+", "VOL-", "RECORD",
                     "BOOT"},
    .button_count = 8,
    .has_power_button = 1,
    .button_group = 100,
    .button_group_mask = 0x7fu,
    .led = 601,
    .battery = 401,
    .motor = 701,
    .vibration_percent = 50,
};
