#ifndef H2_ANDROID_PLATFORM_H
#define H2_ANDROID_PLATFORM_H

#include "h2_pal.h"

#include <android/bitmap.h>
#include <jni.h>

/**
 * Opaque handle for the example-only Android PAL subset.
 *
 * This component currently provides Memory, Time, Task, Queue, Display, Audio
 * playback, H.264/AAC-LC decoding, and a native pointer bridge for portable
 * Apps. It is not a complete Android PAL backend; all other Runtime
 * capabilities remain unsupported until implemented and validated explicitly.
 */
typedef struct h2_android_platform h2_android_platform_t;

typedef struct h2_android_platform_config {
  int32_t display_width;
  int32_t display_height;
} h2_android_platform_config_t;

h2_android_platform_t *
h2_android_platform_create(JNIEnv *env, jobject view,
                           const h2_android_platform_config_t *config);
void h2_android_platform_destroy(h2_android_platform_t *platform);

const h2_pal_mem_api_t *h2_android_platform_mem_api(void);
const h2_pal_time_api_t *h2_android_platform_time_api(void);
const h2_pal_task_api_t *h2_android_platform_task_api(void);
const h2_pal_queue_api_t *h2_android_platform_queue_api(void);
const h2_pal_log_api_t *h2_android_platform_log_api(void);
const h2_pal_audio_api_t *
h2_android_platform_audio_api(h2_android_platform_t *platform);
const h2_pal_audio_decoder_api_t *
h2_android_platform_audio_decoder_api(void);
const h2_pal_video_decoder_api_t *
h2_android_platform_video_decoder_api(void);
const h2_pal_display_api_t *
h2_android_platform_display_api(h2_android_platform_t *platform);

h2_pal_result_t h2_android_platform_read_pointer(void *user, int32_t *out_x,
                                                 int32_t *out_y,
                                                 int *out_pressed);
void h2_android_platform_update_pointer(h2_android_platform_t *platform,
                                        int32_t x, int32_t y, int pressed);
int h2_android_platform_copy_frame(h2_android_platform_t *platform, JNIEnv *env,
                                   jobject bitmap);

#endif
