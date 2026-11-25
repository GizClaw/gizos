#include "h2_mfg.h"
#include "main/recording.h"
#include "main/resources.h"

static const h2_mfg_step_t steps[] = {
    {"storage", H2_MFG_STORAGE, 1}, {"display", H2_MFG_DISPLAY, 0},
    {"buttons", H2_MFG_BUTTONS, 0}, {"battery", H2_MFG_BATTERY, 0},
    {"speaker", H2_MFG_SPEAKER, 0}, {"mic", H2_MFG_MIC, 0},
    {"aec", H2_MFG_AEC, 0},         {"wifi", H2_MFG_WIFI, 1},
    {"ble", H2_MFG_BLE, 1},         {"power", H2_MFG_POWER, 0},
};

const h2_mfg_profile_t h2_mfg_board_profile = {
    .board = "ai_passport",
    /* Keep the unchanged revision-3 checklist and append POWER acceptance. */
    .revision = 4,
    .steps = steps,
    .total = sizeof(steps) / sizeof(steps[0]),
    .append_from_total = 9,
    .append_from_revision = 3,
    .speaker_test_volume = 100,
    .buttons = {103, 102, 101},
    .button_names = {"OK", "UP", "DOWN"},
    .button_count = 3,
    .button_group = 201,
    .battery = 401,
    .display_buffer_rows = 4,
    .storage_ram_bytes = 32768,
    .prepare_check = h2_passport_prepare_check,
    .release_check = h2_passport_release_check,
    .playback_buffer_frames = 2,
    .aec_recording_store = &h2_passport_recording_store,
};
