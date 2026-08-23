#include "h2_game_runtime_internal.hpp"

int h2_game_runtime_send_input(h2_game_runtime_t *runtime, const h2_game_input_event_t *event) {
    if (runtime == nullptr || event == nullptr) {
        return H2_GAME_RUNTIME_ERR_INVALID_ARG;
    }
    if (runtime->input_count >= sizeof(runtime->input_queue) / sizeof(runtime->input_queue[0])) {
        return H2_GAME_RUNTIME_ERR_OVERFLOW;
    }

    const size_t tail = (runtime->input_head + runtime->input_count) %
        (sizeof(runtime->input_queue) / sizeof(runtime->input_queue[0]));
    runtime->input_queue[tail] = *event;
    runtime->input_count += 1;
    return H2_GAME_RUNTIME_OK;
}

void h2_game_runtime_drain_input(h2_game_runtime_t *runtime) {
    if (runtime == nullptr) {
        return;
    }

    if (runtime->input_handler != nullptr && runtime->scene != nullptr) {
        while (runtime->input_count > 0) {
            const h2_game_input_event_t *event = &runtime->input_queue[runtime->input_head];
            runtime->input_handler(reinterpret_cast<h2_game_scene_t *>(runtime->scene), event);
            runtime->input_head = (runtime->input_head + 1) %
                (sizeof(runtime->input_queue) / sizeof(runtime->input_queue[0]));
            runtime->input_count -= 1;
        }
    }

    runtime->input_head = 0;
    runtime->input_count = 0;
}
