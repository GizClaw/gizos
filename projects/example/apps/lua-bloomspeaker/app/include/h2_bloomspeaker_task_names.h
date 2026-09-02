#ifndef H2_BLOOMSPEAKER_TASK_NAMES_H
#define H2_BLOOMSPEAKER_TASK_NAMES_H

#define H2_BLOOMSPEAKER_RUNNER_TASK_NAME_VALUE                    \
  "lua-bloomspeaker/runner"
#define H2_BLOOMSPEAKER_BLE_TASK_NAME_VALUE "lua-bloomspeaker/ble"
#define H2_BLOOMSPEAKER_AUDIO_TASK_NAME_VALUE "lua-bloomspeaker/audio"

#ifdef __cplusplus
extern "C" {
#endif

extern const char h2_bloomspeaker_runner_task_name[sizeof(
    H2_BLOOMSPEAKER_RUNNER_TASK_NAME_VALUE)];
extern const char h2_bloomspeaker_ble_task_name[sizeof(
    H2_BLOOMSPEAKER_BLE_TASK_NAME_VALUE)];
extern const char h2_bloomspeaker_audio_task_name[sizeof(
    H2_BLOOMSPEAKER_AUDIO_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
