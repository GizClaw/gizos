#ifndef H2_AUDIO_MIXER_TYPES_H
#define H2_AUDIO_MIXER_TYPES_H

#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_sync.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_audio_mixer h2_audio_mixer_t;

/** Audio Mixer 初始化参数。 */
typedef struct h2_audio_mixer_config {
    /** 每个 track 接受且 Mixer 输出的 PCM 格式。 */
    h2_audio_pcm_format_t format;
    /** Mixer 最多持有的 track 数量。 */
    uint8_t max_tracks;
    /** 每个 track 的默认 queue 容量，以 PCM frame 为单位。 */
    size_t track_queue_frames;
    /** 以千分比表示的 master gain；零表示使用默认值 1000。 */
    uint32_t master_factor_milli;
    /** 在 Mixer 生命周期内借用的可选 allocator。 */
    const h2_pal_mem_api_t *allocator;
    /** 在 Mixer 生命周期内借用的 queue API。 */
    const h2_pal_queue_api_t *queue_api;
    /** 在 Mixer 生命周期内借用的必填同步 API。 */
    const h2_pal_sync_api_t *sync_api;
} h2_audio_mixer_config_t;

/** Audio Mixer 运行统计信息。 */
typedef struct h2_audio_mixer_stats {
    /** 以千分比表示的有效 master gain。 */
    uint32_t master_factor_milli;
    /** 混合输出中被裁剪到有符号 16-bit 范围的 sample 数量。 */
    uint64_t clip_count;
} h2_audio_mixer_stats_t;

#ifdef __cplusplus
}
#endif

#endif
