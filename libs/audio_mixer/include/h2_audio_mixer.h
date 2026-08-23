#ifndef H2_AUDIO_MIXER_H
#define H2_AUDIO_MIXER_H

#include "h2_audio_mixer_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_audio_mixer {
    /** Mixer 私有实现，调用方不得访问。 */
    void *impl;
} h2_audio_mixer_t;

/**
 * @brief 初始化 Audio Mixer。
 *
 * Mixer 在整个生命周期内借用 config 中的 allocator、queue API 和同步 API。
 *
 * @param mixer 由调用方持有的 Mixer 存储。
 * @param config PCM 格式、容量、allocator、queue 和同步配置。
 * @return 成功时返回 H2_AUDIO_OK，否则返回 audio error result。
 */
int h2_audio_mixer_init(h2_audio_mixer_t *mixer, const h2_audio_mixer_config_t *config);

/**
 * @brief 释放 Mixer 持有的全部 track 和内存。
 *
 * 可以传入 NULL，或对已经 deinit 的 Mixer 重复调用本函数。
 * 调用方必须先停止 playback、producer 和其它 Mixer/track API；本函数不与它们并发。
 *
 * @param mixer 需要 deinit 的 Mixer。
 */
void h2_audio_mixer_deinit(h2_audio_mixer_t *mixer);

/**
 * @brief 创建由 Mixer 消费音频帧的 PAL audio track。
 *
 * 返回的 track 由 Mixer 持有，在 slot 被复用或 Mixer deinit 前保持有效。
 * 可选的 audio 指针会保存在返回的 PAL track 中，表示该 track 所属的 audio object。
 * 创建、关闭和混合读取可以由不同 task 并发调用；关闭一个 track 不会停止或
 * 阻塞其他 active track，且对应 slot 只会在所有在途操作退出后复用。
 * queue 或 condition 失败会让 slot 保持 closing，后续 close/close_all 可以重试；
 * 成功关闭且尚未复用时，重复 close 返回 H2_AUDIO_OK。
 *
 * @param mixer 已初始化的 Mixer。
 * @param audio 与返回 track 关联的 audio object，可以为 NULL。
 * @param config Track 格式、buffer、名称和音量配置。
 * @param out_track 接收指向 Mixer-owned PAL audio track 的借用指针。
 * @return 成功时返回 H2_AUDIO_OK，否则返回 audio error result。
 */
int h2_audio_mixer_create_track(
    h2_audio_mixer_t *mixer,
    const h2_pal_audio_api_t *audio,
    const h2_audio_track_config_t *config,
    h2_pal_audio_track_t **out_track);

/**
 * @brief 从每个 active track 读取一个排队帧并混合到输出帧。
 *
 * 本函数与 track 创建和关闭串行化，但不会等待 track producer 写入数据。
 *
 * @param mixer 已初始化的 Mixer。
 * @param out_frame 调用方持有的输出 frame 和存储；本函数会更新其 bytes 字段。
 * @return 成功时返回 H2_AUDIO_OK，否则返回 audio error result。
 */
int h2_audio_mixer_read(h2_audio_mixer_t *mixer, h2_audio_frame_t *out_frame);

/**
 * @brief 混合一个排队帧，同时写入 reference frame。
 *
 * @param mixer 已初始化的 Mixer。
 * @param out_frame 调用方持有的混合输出 frame 和存储。
 * @param ref_frame 可选的、由调用方持有的 reference frame 和存储，可以为 NULL。
 * @return 成功时返回 H2_AUDIO_OK，否则返回 audio error result。
 */
int h2_audio_mixer_read_with_reference(
    h2_audio_mixer_t *mixer,
    h2_audio_frame_t *out_frame,
    h2_audio_frame_t *ref_frame);

/**
 * @brief 关闭 Mixer 持有的全部 track。
 *
 * 调用期间拒绝创建新 track，并尝试收敛 active 或先前关闭失败的 track。
 * 单个 track 失败不会跳过其它 track；失败的 slot 保持 closing，后续可以重试。
 *
 * @param mixer 已初始化的 Mixer。
 * @return 全部 track 成功关闭时返回 H2_AUDIO_OK，否则返回第一个 audio error result。
 */
int h2_audio_mixer_close_all(h2_audio_mixer_t *mixer);

/**
 * @brief 读取当前 Mixer 统计信息。
 *
 * @param mixer 已初始化的 Mixer。
 * @param out_stats 接收 Mixer 统计信息快照。
 * @return 成功时返回 H2_AUDIO_OK，否则返回 H2_AUDIO_ERR_INVALID_ARG。
 */
int h2_audio_mixer_get_stats(h2_audio_mixer_t *mixer, h2_audio_mixer_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif
