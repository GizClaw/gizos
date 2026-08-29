#ifndef H2_SMOKE_AUDIO_SYSTEM_TASK_NAMES_H
#define H2_SMOKE_AUDIO_SYSTEM_TASK_NAMES_H

#define H2_SMOKE_AUDIO_SYSTEM_MUSIC_TASK_NAME_VALUE "audio-system/music"
#define H2_SMOKE_AUDIO_SYSTEM_MIC_TASK_NAME_VALUE "audio-system/mic"

#ifdef __cplusplus
extern "C" {
#endif

extern const char h2_smoke_audio_system_music_task_name[sizeof(
    H2_SMOKE_AUDIO_SYSTEM_MUSIC_TASK_NAME_VALUE)];
extern const char h2_smoke_audio_system_mic_task_name[sizeof(
    H2_SMOKE_AUDIO_SYSTEM_MIC_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
