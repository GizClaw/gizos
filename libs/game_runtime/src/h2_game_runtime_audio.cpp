#include "h2_game_audio.h"
#include "h2_game_runtime_task_names.h"

#include "audio/DefaultAudioScheduler.h"

#include <atomic>
#include <new>

namespace {
constexpr uint32_t kBlockSamples = 128;
constexpr uint32_t kWriteTimeoutMs = 100;

struct AudioCommand {
    const h2_game_audio_recipe_t *recipe;
};

pixelroot32::audio::WaveType wave_type(h2_game_audio_wave_t wave) {
    using WaveType = pixelroot32::audio::WaveType;
    switch (wave) {
        case H2_GAME_AUDIO_WAVE_TRIANGLE: return WaveType::TRIANGLE;
        case H2_GAME_AUDIO_WAVE_NOISE: return WaveType::NOISE;
        case H2_GAME_AUDIO_WAVE_SINE: return WaveType::SINE;
        case H2_GAME_AUDIO_WAVE_SAW: return WaveType::SAW;
        case H2_GAME_AUDIO_WAVE_PULSE:
        default: return WaveType::PULSE;
    }
}

bool valid_recipe(const h2_game_audio_recipe_t *recipe) {
    if (recipe == nullptr || recipe->steps == nullptr || recipe->step_count == 0u || recipe->duration_ms == 0u) {
        return false;
    }
    for (size_t index = 0; index < recipe->step_count; ++index) {
        const auto &step = recipe->steps[index];
        if (index > 0u && step.start_ms < recipe->steps[index - 1u].start_ms) return false;
        if (step.start_ms >= recipe->duration_ms || step.duration_ms > recipe->duration_ms - step.start_ms) {
            return false;
        }
    }
    return true;
}
}

struct h2_game_audio {
    explicit h2_game_audio(const h2_game_audio_config_t &value)
        : config(value) {}

    h2_game_audio_config_t config;
    pixelroot32::audio::DefaultAudioScheduler scheduler;
    h2_audio_pcm_format_t playback_format{};
    int16_t *samples = nullptr;
    h2_pal_audio_track_t *track = nullptr;
    h2_pal_queue_t *commands = nullptr;
    h2_pal_task_t *worker = nullptr;
    std::atomic<const h2_game_audio_recipe_t *> priority_recipe{nullptr};
    std::atomic<bool> stopping{false};
    std::atomic<int> worker_status{H2_GAME_AUDIO_OK};
};

namespace {
uint64_t sample_at_ms(uint32_t ms) {
    return static_cast<uint64_t>(ms) * H2_GAME_AUDIO_SAMPLE_RATE_HZ / 1000u;
}

void play_step(h2_game_audio_t *audio, const h2_game_audio_step_t &step) {
    pixelroot32::audio::AudioEvent event{};
    event.type = wave_type(step.wave);
    event.frequency = static_cast<float>(step.frequency_hz);
    event.duration = static_cast<float>(step.duration_ms) / 1000.0f;
    event.volume = static_cast<float>(step.volume_permille) / 1000.0f;
    event.duty = static_cast<float>(step.duty_permille) / 1000.0f;
    if (step.end_frequency_hz != 0u && step.end_frequency_hz != step.frequency_hz) {
        event.sweepEndHz = static_cast<float>(step.end_frequency_hz);
        event.sweepDurationSec = event.duration;
    }
    pixelroot32::audio::AudioCommand command{};
    command.type = pixelroot32::audio::AudioCommandType::PLAY_EVENT;
    command.event = event;
    audio->scheduler.submitCommand(command);
}

void generate_recipe_block(
    h2_game_audio_t *audio,
    const h2_game_audio_recipe_t *recipe,
    uint64_t *cursor,
    size_t *next_step,
    int16_t *samples,
    size_t count) {
    size_t produced = 0;
    while (produced < count) {
        uint64_t boundary = *cursor + (count - produced);
        if (recipe != nullptr && *next_step < recipe->step_count) {
            const uint64_t step_sample = sample_at_ms(recipe->steps[*next_step].start_ms);
            if (step_sample <= *cursor) {
                play_step(audio, recipe->steps[*next_step]);
                *next_step += 1;
                continue;
            }
            if (step_sample < boundary) boundary = step_sample;
        }
        const size_t segment = static_cast<size_t>(boundary - *cursor);
        audio->scheduler.generateSamples(samples + produced, static_cast<int>(segment));
        produced += segment;
        *cursor += segment;
    }
}

void discard_queued_commands(h2_game_audio_t *audio) {
    AudioCommand command{};
    for (size_t index = 0; index < audio->config.command_capacity; ++index) {
        if (h2_pal_queue_recv(
                audio->config.queue,
                audio->commands,
                &command,
                H2_PAL_QUEUE_NO_WAIT) != H2_PAL_OK) return;
    }
}

void stop_active_voices(h2_game_audio_t *audio) {
    for (uint8_t channel = 0; channel < pixelroot32::audio::ApuCore::MAX_VOICES; ++channel) {
        pixelroot32::audio::AudioCommand command{};
        command.type = pixelroot32::audio::AudioCommandType::STOP_CHANNEL;
        command.channelIndex = channel;
        audio->scheduler.submitCommand(command);
    }
}

void audio_worker(void *ctx) {
    auto *audio = static_cast<h2_game_audio_t *>(ctx);
    const h2_game_audio_recipe_t *recipe = nullptr;
    uint64_t cursor = 0;
    size_t next_step = 0;
    const size_t frame_samples = audio->playback_format.frame_samples_per_channel;
    const size_t frame_bytes = frame_samples * sizeof(*audio->samples);
    bool frame_pending = false;
    bool priority_active = false;

    while (!audio->stopping.load()) {
        if (!frame_pending) {
            AudioCommand command{};
            command.recipe = audio->priority_recipe.exchange(nullptr);
            if (command.recipe != nullptr) {
                stop_active_voices(audio);
                recipe = command.recipe;
                cursor = 0;
                next_step = 0;
                priority_active = true;
            }
            if (priority_active) {
                discard_queued_commands(audio);
            } else if (h2_pal_queue_recv(
                           audio->config.queue,
                           audio->commands,
                           &command,
                           H2_PAL_QUEUE_NO_WAIT) == H2_PAL_OK &&
                command.recipe != nullptr) {
                recipe = command.recipe;
                cursor = 0;
                next_step = 0;
            }

            generate_recipe_block(audio, recipe, &cursor, &next_step, audio->samples, frame_samples);
            if (recipe != nullptr && cursor >= sample_at_ms(recipe->duration_ms)) recipe = nullptr;
            frame_pending = true;
        }

        h2_audio_frame_t frame = h2_audio_frame_for_buffer(
            audio->samples,
            frame_bytes,
            audio->playback_format);
        frame.bytes = frame_bytes;
        const int rc = h2_pal_audio_track_write(audio->track, &frame, kWriteTimeoutMs);
        if (rc == H2_AUDIO_ERR_WOULD_BLOCK) continue;
        if (rc != H2_AUDIO_OK) {
            audio->worker_status.store(H2_GAME_AUDIO_ERR_AUDIO);
            audio->stopping.store(true);
            continue;
        }
        frame_pending = false;
        if (priority_active && recipe == nullptr) {
            discard_queued_commands(audio);
            priority_active = false;
        }
    }
}
}

int h2_game_audio_create(const h2_game_audio_config_t *config, h2_game_audio_t **out_audio) {
    if (out_audio == nullptr) return H2_GAME_AUDIO_ERR_INVALID_ARG;
    *out_audio = nullptr;
    if (config == nullptr || config->audio == nullptr || config->task == nullptr ||
        config->queue == nullptr || config->mem == nullptr || config->command_capacity == 0u) {
        return H2_GAME_AUDIO_ERR_INVALID_ARG;
    }
    h2_audio_info_t info{};
    if (h2_pal_audio_get_info(config->audio, &info) != H2_AUDIO_OK ||
        !info.available || !info.playback_supported ||
        info.playback_format.sample_rate_hz != H2_GAME_AUDIO_SAMPLE_RATE_HZ ||
        info.playback_format.frame_samples_per_channel == 0u ||
        info.playback_format.channels != 1u ||
        info.playback_format.sample_format != H2_AUDIO_SAMPLE_S16LE) {
        return H2_GAME_AUDIO_ERR_AUDIO;
    }
    void *memory = h2_pal_mem_alloc(config->mem, sizeof(h2_game_audio_t));
    if (memory == nullptr) return H2_GAME_AUDIO_ERR_NO_MEMORY;
    auto *audio = new (memory) h2_game_audio_t(*config);
    audio->playback_format = info.playback_format;
    audio->samples = static_cast<int16_t *>(h2_pal_mem_alloc(
        config->mem,
        static_cast<size_t>(audio->playback_format.frame_samples_per_channel) * sizeof(*audio->samples)));
    if (audio->samples == nullptr) {
        audio->~h2_game_audio(); h2_pal_mem_free(config->mem, memory); return H2_GAME_AUDIO_ERR_NO_MEMORY;
    }

    h2_audio_track_config_t track_config = {
        "pixa-game-sfx",
        audio->playback_format,
        1000,
        4,
    };
    if (h2_pal_audio_create_track(config->audio, &track_config, &audio->track) != H2_AUDIO_OK) {
        h2_pal_mem_free(config->mem, audio->samples);
        audio->~h2_game_audio(); h2_pal_mem_free(config->mem, memory); return H2_GAME_AUDIO_ERR_AUDIO;
    }
    const h2_pal_queue_config_t queue_config = {"pixa-game-sfx", sizeof(AudioCommand), config->command_capacity, config->mem};
    if (h2_pal_queue_create(config->queue, &queue_config, &audio->commands) != H2_PAL_OK) {
        h2_pal_audio_track_close(audio->track);
        h2_pal_mem_free(config->mem, audio->samples);
        audio->~h2_game_audio(); h2_pal_mem_free(config->mem, memory); return H2_GAME_AUDIO_ERR_QUEUE;
    }
    audio->scheduler.init(
        nullptr,
        H2_GAME_AUDIO_SAMPLE_RATE_HZ,
        pixelroot32::platforms::PlatformCapabilities(),
        kBlockSamples);
    audio->scheduler.start();
    const h2_pal_task_options_t options = {h2_game_runtime_audio_task_name, 8192};
    if (h2_pal_task_start(config->task, &options, audio_worker, audio, &audio->worker) != H2_PAL_OK) {
        h2_pal_queue_destroy(config->queue, audio->commands); h2_pal_audio_track_close(audio->track);
        h2_pal_mem_free(config->mem, audio->samples);
        audio->~h2_game_audio(); h2_pal_mem_free(config->mem, memory);
        return H2_GAME_AUDIO_ERR_TASK;
    }
    *out_audio = audio;
    return H2_GAME_AUDIO_OK;
}

int h2_game_audio_play(h2_game_audio_t *audio, const h2_game_audio_recipe_t *recipe) {
    if (audio == nullptr || !valid_recipe(recipe)) return H2_GAME_AUDIO_ERR_INVALID_ARG;
    const AudioCommand command = {recipe};
    return h2_pal_queue_send(audio->config.queue, audio->commands, &command, H2_PAL_QUEUE_NO_WAIT) == H2_PAL_OK
        ? H2_GAME_AUDIO_OK : H2_GAME_AUDIO_ERR_OVERFLOW;
}

int h2_game_audio_play_latest(h2_game_audio_t *audio, const h2_game_audio_recipe_t *recipe) {
    if (audio == nullptr || !valid_recipe(recipe)) {
        return H2_GAME_AUDIO_ERR_INVALID_ARG;
    }
    audio->priority_recipe.store(recipe);
    return H2_GAME_AUDIO_OK;
}

int h2_game_audio_stop(h2_game_audio_t *audio) {
    if (audio == nullptr) return H2_GAME_AUDIO_ERR_INVALID_ARG;
    audio->stopping.store(true);
    if (audio->worker != nullptr) {
        if (h2_pal_task_join(audio->config.task, audio->worker) != H2_PAL_OK) return H2_GAME_AUDIO_ERR_TASK;
        audio->worker = nullptr;
    }
    audio->scheduler.stop();
    return audio->worker_status.load();
}

int h2_game_audio_destroy(h2_game_audio_t *audio) {
    if (audio == nullptr) return H2_GAME_AUDIO_ERR_INVALID_ARG;
    const int stop_rc = h2_game_audio_stop(audio);
    if (stop_rc == H2_GAME_AUDIO_ERR_TASK) return stop_rc;
    if (audio->commands != nullptr) h2_pal_queue_destroy(audio->config.queue, audio->commands);
    if (audio->track != nullptr) (void)h2_pal_audio_track_close(audio->track);
    const h2_pal_mem_api_t *mem = audio->config.mem;
    h2_pal_mem_free(mem, audio->samples);
    audio->~h2_game_audio();
    h2_pal_mem_free(mem, audio);
    return stop_rc;
}
