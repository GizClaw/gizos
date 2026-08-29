#ifndef H2_SMOKE_MP4_PLAYER_TASK_NAMES_H
#define H2_SMOKE_MP4_PLAYER_TASK_NAMES_H

#define H2_SMOKE_MP4_PLAYER_AUDIO_TASK_NAME_VALUE "mp4-player/audio"
#define H2_SMOKE_MP4_PLAYER_DECODER_TASK_NAME_VALUE "mp4-player/decoder"

#ifdef __cplusplus
extern "C" {
#endif

extern const char h2_smoke_mp4_player_audio_task_name[sizeof(
    H2_SMOKE_MP4_PLAYER_AUDIO_TASK_NAME_VALUE)];
extern const char h2_smoke_mp4_player_decoder_task_name[sizeof(
    H2_SMOKE_MP4_PLAYER_DECODER_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
