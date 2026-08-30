#ifndef H2_PAL_AUDIO_TASK_NAMES_H
#define H2_PAL_AUDIO_TASK_NAMES_H

#define H2_PAL_AUDIO_MIC_TASK_NAME_VALUE "$audio/mic"
#define H2_PAL_AUDIO_MIX_TASK_NAME_VALUE "$audio/mix"

#ifdef __cplusplus
extern "C" {
#endif

extern const char h2_pal_audio_mic_task_name[
    sizeof(H2_PAL_AUDIO_MIC_TASK_NAME_VALUE)];
extern const char h2_pal_audio_mix_task_name[
    sizeof(H2_PAL_AUDIO_MIX_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
