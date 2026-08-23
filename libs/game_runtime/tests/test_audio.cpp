#include "h2_game_audio.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

struct h2_pal_queue {
    size_t item_size = 0;
    size_t item_count = 0;
    std::mutex mutex;
    std::deque<std::vector<uint8_t>> items;
};

struct h2_pal_task {
    std::thread thread;
};

struct TestTask {
    std::atomic<bool> fail_next_join{false};
};

struct TestAudio {
    h2_pal_audio_api_t api{};
    h2_pal_audio_track_t track{};
    h2_audio_pcm_format_t format{H2_GAME_AUDIO_SAMPLE_RATE_HZ, 320, 1, H2_AUDIO_SAMPLE_S16LE};
    std::atomic<size_t> writes{0};
    std::atomic<size_t> nonzero_samples{0};
    std::atomic<size_t> would_blocks{0};
    std::atomic<bool> fail_next_write{false};
    std::atomic<bool> retried_blocked_frame{false};
    std::atomic<bool> speaker_started{false};
    std::atomic<bool> hold_writes{false};
    std::atomic<bool> write_entered{false};
    std::mutex write_mutex;
    std::condition_variable write_condition;
    std::mutex frames_mutex;
    std::vector<int16_t> blocked_frame;
    std::vector<std::vector<int16_t>> frames;
};

namespace {
void *mem_alloc(void *, size_t len) { return std::malloc(len); }
void *mem_realloc(void *, void *ptr, size_t len) { return std::realloc(ptr, len); }
void mem_free(void *, void *ptr) { std::free(ptr); }

int queue_create(void *, const h2_pal_queue_config_t *config, h2_pal_queue_t **out) {
    auto *queue = new (std::nothrow) h2_pal_queue_t;
    if (queue == nullptr) return H2_PAL_ERR_NO_MEMORY;
    queue->item_size = config->item_size;
    queue->item_count = config->item_count;
    *out = queue;
    return H2_PAL_OK;
}
void queue_destroy(void *, h2_pal_queue_t *queue) { delete queue; }
int queue_send(void *, h2_pal_queue_t *queue, const void *item, uint32_t) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->items.size() >= queue->item_count) return H2_PAL_ERR_FULL;
    queue->items.emplace_back(queue->item_size);
    std::memcpy(queue->items.back().data(), item, queue->item_size);
    return H2_PAL_OK;
}
int queue_recv(void *, h2_pal_queue_t *queue, void *out, uint32_t) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->items.empty()) return H2_PAL_ERR_WOULD_BLOCK;
    std::memcpy(out, queue->items.front().data(), queue->item_size);
    queue->items.pop_front();
    return H2_PAL_OK;
}
int task_start(void *, const h2_pal_task_options_t *, h2_pal_task_entry_t entry, void *ctx, h2_pal_task_t **out) {
    auto *task = new (std::nothrow) h2_pal_task_t{std::thread(entry, ctx)};
    if (task == nullptr) return H2_PAL_ERR_NO_MEMORY;
    *out = task;
    return H2_PAL_OK;
}
int task_join(void *user, h2_pal_task_t *task) {
    auto *state = static_cast<TestTask *>(user);
    if (state->fail_next_join.exchange(false)) return H2_PAL_ERR_IO;
    task->thread.join();
    delete task;
    return H2_PAL_OK;
}

int start_speaker(void *user) {
    static_cast<TestAudio *>(user)->speaker_started.store(true);
    return H2_AUDIO_OK;
}
int stop_speaker(void *user) {
    static_cast<TestAudio *>(user)->speaker_started.store(false);
    return H2_AUDIO_OK;
}
int get_info(void *user, h2_audio_info_t *info) {
    auto *audio = static_cast<TestAudio *>(user);
    *info = {};
    info->available = 1;
    info->playback_supported = 1;
    info->playback_format = audio->format;
    info->track_queue_frames = 4;
    info->max_tracks = 4;
    return H2_AUDIO_OK;
}
int track_write(h2_pal_audio_track_t *track, const h2_audio_frame_t *frame, uint32_t) {
    auto *audio = static_cast<TestAudio *>(track->user);
    assert(frame->sample_rate_hz == audio->format.sample_rate_hz);
    assert(frame->samples_per_channel == audio->format.frame_samples_per_channel);
    assert(frame->channels == audio->format.channels);
    assert(frame->sample_format == audio->format.sample_format);
    assert(frame->bytes == static_cast<size_t>(audio->format.frame_samples_per_channel) * sizeof(int16_t));
    assert(frame->capacity >= frame->bytes);
    if (audio->hold_writes.load()) {
        audio->write_entered.store(true);
        audio->write_condition.notify_all();
        std::unique_lock<std::mutex> lock(audio->write_mutex);
        audio->write_condition.wait(lock, [audio]() { return !audio->hold_writes.load(); });
    }
    const auto *samples = static_cast<const int16_t *>(frame->data);
    std::vector<int16_t> copied_samples(
        samples,
        samples + audio->format.frame_samples_per_channel);
    if (audio->fail_next_write.exchange(false)) {
        std::lock_guard<std::mutex> lock(audio->frames_mutex);
        audio->blocked_frame = copied_samples;
        audio->would_blocks.fetch_add(1);
        return H2_AUDIO_ERR_WOULD_BLOCK;
    }
    {
        std::lock_guard<std::mutex> lock(audio->frames_mutex);
        if (!audio->blocked_frame.empty()) {
            audio->retried_blocked_frame.store(audio->blocked_frame == copied_samples);
            audio->blocked_frame.clear();
        }
        audio->frames.push_back(copied_samples);
    }
    for (size_t i = 0; i < audio->format.frame_samples_per_channel; ++i) {
        if (samples[i] != 0) audio->nonzero_samples.fetch_add(1);
    }
    audio->writes.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return H2_AUDIO_OK;
}
int track_close(h2_pal_audio_track_t *) { return H2_AUDIO_OK; }
int create_track(void *user, const h2_audio_track_config_t *config, h2_pal_audio_track_t **out) {
    auto *audio = static_cast<TestAudio *>(user);
    assert(config->format.sample_rate_hz == audio->format.sample_rate_hz);
    assert(config->format.frame_samples_per_channel == audio->format.frame_samples_per_channel);
    assert(config->format.channels == audio->format.channels);
    assert(config->format.sample_format == audio->format.sample_format);
    audio->track = {
        audio, &audio->api, track_write, track_close, nullptr, nullptr, nullptr};
    *out = &audio->track;
    return H2_AUDIO_OK;
}

void run_audio_case(uint16_t frame_samples, bool test_join_retry) {
    static const h2_pal_mem_vtable_t mem_vtable = {mem_alloc, mem_realloc, mem_free};
    const h2_pal_mem_api_t mem = {nullptr, &mem_vtable};
    static const h2_pal_queue_vtable_t queue_vtable = {
        queue_create, queue_destroy, queue_send, nullptr, queue_recv, nullptr, nullptr,
    };
    const h2_pal_queue_api_t queue = {nullptr, &queue_vtable};
    static const h2_pal_task_vtable_t task_vtable = {task_start, task_join};
    TestTask task_state;
    const h2_pal_task_api_t task = {&task_state, &task_vtable};
    static const h2_pal_audio_vtable_t audio_vtable = {
        get_info, nullptr, nullptr, start_speaker, stop_speaker, nullptr, create_track, nullptr, nullptr,
    };
    TestAudio output;
    output.format.frame_samples_per_channel = frame_samples;
    output.api = {&output, &audio_vtable};
    assert(h2_pal_audio_start_speaker(&output.api) == H2_AUDIO_OK);

    h2_game_audio_t *audio = nullptr;
    const h2_game_audio_config_t config = {&output.api, &task, &queue, &mem, 4};
    assert(h2_game_audio_create(&config, &audio) == H2_GAME_AUDIO_OK);
    assert(audio != nullptr && output.speaker_started.load());
    output.fail_next_write.store(true);
    assert(h2_game_audio_play(audio, nullptr) == H2_GAME_AUDIO_ERR_INVALID_ARG);

    static const h2_game_audio_step_t invalid_steps[] = {
        {20, 1, 440, 440, 500, 500, H2_GAME_AUDIO_WAVE_SINE},
    };
    static const h2_game_audio_recipe_t step_at_end = {invalid_steps, 1, 20};
    static const h2_game_audio_step_t overflowing_steps[] = {
        {19, 2, 440, 440, 500, 500, H2_GAME_AUDIO_WAVE_SINE},
    };
    static const h2_game_audio_recipe_t step_past_end = {overflowing_steps, 1, 20};
    static const h2_game_audio_step_t unordered_steps[] = {
        {10, 1, 440, 440, 500, 500, H2_GAME_AUDIO_WAVE_SINE},
        {0, 1, 440, 440, 500, 500, H2_GAME_AUDIO_WAVE_SINE},
    };
    static const h2_game_audio_recipe_t unordered_recipe = {unordered_steps, 2, 20};
    assert(h2_game_audio_play(audio, &step_at_end) == H2_GAME_AUDIO_ERR_INVALID_ARG);
    assert(h2_game_audio_play_latest(audio, &step_at_end) == H2_GAME_AUDIO_ERR_INVALID_ARG);
    assert(h2_game_audio_play(audio, &step_past_end) == H2_GAME_AUDIO_ERR_INVALID_ARG);
    assert(h2_game_audio_play_latest(audio, &step_past_end) == H2_GAME_AUDIO_ERR_INVALID_ARG);
    assert(h2_game_audio_play(audio, &unordered_recipe) == H2_GAME_AUDIO_ERR_INVALID_ARG);
    assert(h2_game_audio_play_latest(audio, &unordered_recipe) == H2_GAME_AUDIO_ERR_INVALID_ARG);

    static const h2_game_audio_step_t steps[] = {
        {3, 12, 220, 660, 700, 500, H2_GAME_AUDIO_WAVE_SINE},
        {17, 8, 900, 450, 500, 500, H2_GAME_AUDIO_WAVE_PULSE},
    };
    static const h2_game_audio_recipe_t recipe = {steps, 2, 30};
    assert(h2_game_audio_play(audio, &recipe) == H2_GAME_AUDIO_OK);
    assert(h2_game_audio_play_latest(audio, &recipe) == H2_GAME_AUDIO_OK);
    for (int i = 0; i < 200 && (output.nonzero_samples.load() == 0 || output.writes.load() < 2); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(output.writes.load() >= 2);
    assert(output.nonzero_samples.load() > 0);
    assert(output.would_blocks.load() == 1);
    assert(output.retried_blocked_frame.load());
    if (test_join_retry) {
        task_state.fail_next_join.store(true);
        assert(h2_game_audio_destroy(audio) == H2_GAME_AUDIO_ERR_TASK);
        assert(output.speaker_started.load());
    }
    assert(h2_game_audio_destroy(audio) == H2_GAME_AUDIO_OK);
    assert(output.speaker_started.load());
    assert(h2_pal_audio_stop_speaker(&output.api) == H2_AUDIO_OK);
    assert(!output.speaker_started.load());
}

std::vector<std::vector<int16_t>> capture_priority_frames(bool queue_regular_recipe) {
    static const h2_pal_mem_vtable_t mem_vtable = {mem_alloc, mem_realloc, mem_free};
    const h2_pal_mem_api_t mem = {nullptr, &mem_vtable};
    static const h2_pal_queue_vtable_t queue_vtable = {
        queue_create, queue_destroy, queue_send, nullptr, queue_recv, nullptr, nullptr,
    };
    const h2_pal_queue_api_t queue = {nullptr, &queue_vtable};
    static const h2_pal_task_vtable_t task_vtable = {task_start, task_join};
    TestTask task_state;
    const h2_pal_task_api_t task = {&task_state, &task_vtable};
    static const h2_pal_audio_vtable_t audio_vtable = {
        get_info, nullptr, nullptr, start_speaker, stop_speaker, nullptr, create_track, nullptr, nullptr,
    };
    TestAudio output;
    output.hold_writes.store(true);
    output.api = {&output, &audio_vtable};
    assert(h2_pal_audio_start_speaker(&output.api) == H2_AUDIO_OK);

    h2_game_audio_t *audio = nullptr;
    const h2_game_audio_config_t config = {&output.api, &task, &queue, &mem, 4};
    assert(h2_game_audio_create(&config, &audio) == H2_GAME_AUDIO_OK);
    {
        std::unique_lock<std::mutex> lock(output.write_mutex);
        assert(output.write_condition.wait_for(
            lock,
            std::chrono::seconds(1),
            [&output]() { return output.write_entered.load(); }));
    }

    static const h2_game_audio_step_t regular_steps[] = {
        {0, 8, 1100, 500, 900, 500, H2_GAME_AUDIO_WAVE_PULSE},
    };
    static const h2_game_audio_recipe_t regular_recipe = {regular_steps, 1, 20};
    static const h2_game_audio_step_t priority_steps[] = {
        {40, 12, 440, 880, 700, 500, H2_GAME_AUDIO_WAVE_SINE},
    };
    static const h2_game_audio_recipe_t priority_recipe = {priority_steps, 1, 70};
    if (queue_regular_recipe) assert(h2_game_audio_play(audio, &regular_recipe) == H2_GAME_AUDIO_OK);
    assert(h2_game_audio_play_latest(audio, &priority_recipe) == H2_GAME_AUDIO_OK);
    output.hold_writes.store(false);
    output.write_condition.notify_all();

    for (int i = 0; i < 1000; ++i) {
        {
            std::lock_guard<std::mutex> lock(output.frames_mutex);
            if (output.frames.size() >= 6) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::vector<std::vector<int16_t>> frames;
    {
        std::lock_guard<std::mutex> lock(output.frames_mutex);
        assert(output.frames.size() >= 6);
        frames.assign(output.frames.begin(), output.frames.begin() + 6);
    }
    assert(h2_game_audio_destroy(audio) == H2_GAME_AUDIO_OK);
    assert(h2_pal_audio_stop_speaker(&output.api) == H2_AUDIO_OK);
    return frames;
}

void test_priority_recipe_discards_queued_effects() {
    const auto priority_only = capture_priority_frames(false);
    const auto priority_after_regular = capture_priority_frames(true);
    assert(priority_only == priority_after_regular);
}

int32_t peak_abs(const std::vector<int16_t> &frame) {
    int32_t peak = 0;
    for (const int16_t sample : frame) {
        const int32_t value = sample < 0 ? -static_cast<int32_t>(sample) : static_cast<int32_t>(sample);
        if (value > peak) peak = value;
    }
    return peak;
}

void test_priority_recipe_stops_active_effects() {
    static const h2_pal_mem_vtable_t mem_vtable = {mem_alloc, mem_realloc, mem_free};
    const h2_pal_mem_api_t mem = {nullptr, &mem_vtable};
    static const h2_pal_queue_vtable_t queue_vtable = {
        queue_create, queue_destroy, queue_send, nullptr, queue_recv, nullptr, nullptr,
    };
    const h2_pal_queue_api_t queue = {nullptr, &queue_vtable};
    static const h2_pal_task_vtable_t task_vtable = {task_start, task_join};
    TestTask task_state;
    const h2_pal_task_api_t task = {&task_state, &task_vtable};
    static const h2_pal_audio_vtable_t audio_vtable = {
        get_info, nullptr, nullptr, start_speaker, stop_speaker, nullptr, create_track, nullptr, nullptr,
    };
    TestAudio output;
    output.hold_writes.store(true);
    output.api = {&output, &audio_vtable};
    assert(h2_pal_audio_start_speaker(&output.api) == H2_AUDIO_OK);

    h2_game_audio_t *audio = nullptr;
    const h2_game_audio_config_t config = {&output.api, &task, &queue, &mem, 4};
    assert(h2_game_audio_create(&config, &audio) == H2_GAME_AUDIO_OK);
    {
        std::unique_lock<std::mutex> lock(output.write_mutex);
        assert(output.write_condition.wait_for(
            lock,
            std::chrono::seconds(1),
            [&output]() { return output.write_entered.load(); }));
    }

    static const h2_game_audio_step_t active_steps[] = {
        {0, 2000, 880, 880, 900, 500, H2_GAME_AUDIO_WAVE_SINE},
    };
    static const h2_game_audio_recipe_t active_recipe = {active_steps, 1, 2000};
    assert(h2_game_audio_play(audio, &active_recipe) == H2_GAME_AUDIO_OK);
    output.hold_writes.store(false);
    output.write_condition.notify_all();
    for (int i = 0; i < 1000 && output.nonzero_samples.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(output.nonzero_samples.load() > 0);

    output.write_entered.store(false);
    output.hold_writes.store(true);
    {
        std::unique_lock<std::mutex> lock(output.write_mutex);
        assert(output.write_condition.wait_for(
            lock,
            std::chrono::seconds(1),
            [&output]() { return output.write_entered.load(); }));
    }
    size_t first_pending_frame = 0;
    {
        std::lock_guard<std::mutex> lock(output.frames_mutex);
        first_pending_frame = output.frames.size();
    }
    static const h2_game_audio_step_t priority_steps[] = {
        {0, 100, 440, 440, 0, 500, H2_GAME_AUDIO_WAVE_SINE},
    };
    static const h2_game_audio_recipe_t priority_recipe = {priority_steps, 1, 100};
    assert(h2_game_audio_play_latest(audio, &priority_recipe) == H2_GAME_AUDIO_OK);
    output.hold_writes.store(false);
    output.write_condition.notify_all();

    for (int i = 0; i < 1000; ++i) {
        bool enough_frames = false;
        {
            std::lock_guard<std::mutex> lock(output.frames_mutex);
            enough_frames = output.frames.size() >= first_pending_frame + 7;
        }
        if (enough_frames) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        std::lock_guard<std::mutex> lock(output.frames_mutex);
        assert(output.frames.size() >= first_pending_frame + 7);
        const int32_t active_peak = peak_abs(output.frames[first_pending_frame]);
        const int32_t preempted_peak = peak_abs(output.frames[first_pending_frame + 6]);
        assert(active_peak > 1000);
        assert(preempted_peak < active_peak / 20);
    }
    assert(h2_game_audio_destroy(audio) == H2_GAME_AUDIO_OK);
    assert(h2_pal_audio_stop_speaker(&output.api) == H2_AUDIO_OK);
}

void test_create_clears_output_on_invalid_config() {
    h2_game_audio_t *audio = reinterpret_cast<h2_game_audio_t *>(1);
    const h2_game_audio_config_t config{};
    assert(h2_game_audio_create(&config, &audio) == H2_GAME_AUDIO_ERR_INVALID_ARG);
    assert(audio == nullptr);
}
}

int main() {
    test_create_clears_output_on_invalid_config();
    test_priority_recipe_discards_queued_effects();
    test_priority_recipe_stops_active_effects();
    run_audio_case(256, false);
    run_audio_case(320, true);
    run_audio_case(512, false);
    return 0;
}
