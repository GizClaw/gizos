#include "h2_game_runtime_internal.hpp"

#include <cassert>

namespace {
int delivered[16];
size_t delivered_count;

void handle(h2_game_scene_t *, const h2_game_input_event_t *event) {
    delivered[delivered_count++] = static_cast<int>(event->type);
}
}

int main() {
    static_assert(H2_GAME_INPUT_BUTTON_DOWN == 3);
    static_assert(H2_GAME_INPUT_BUTTON_UP == 4);
    static_assert(H2_GAME_INPUT_BUTTON_CLICK == 5);

    // Queue behavior is covered through the public implementation without creating a display.
    h2_game_runtime runtime = {};
    runtime.scene = reinterpret_cast<pixelroot32::core::Scene *>(&runtime);
    runtime.input_handler = handle;
    const h2_game_input_event_t events[] = {
        {H2_GAME_INPUT_BUTTON_DOWN, 0, 0, 1},
        {H2_GAME_INPUT_BUTTON_CLICK, 0, 0, 1},
        {H2_GAME_INPUT_BUTTON_UP, 0, 0, 1},
    };
    for (const auto &event : events) assert(h2_game_runtime_send_input(&runtime, &event) == H2_GAME_RUNTIME_OK);
    assert(delivered_count == 0);
    h2_game_runtime_drain_input(&runtime);
    assert(delivered_count == 3);
    for (size_t i = 0; i < 3; ++i) assert(delivered[i] == static_cast<int>(events[i].type));

    for (size_t i = 0; i < 16; ++i) assert(h2_game_runtime_send_input(&runtime, &events[1]) == H2_GAME_RUNTIME_OK);
    assert(h2_game_runtime_send_input(&runtime, &events[1]) == H2_GAME_RUNTIME_ERR_OVERFLOW);
    runtime.input_handler = nullptr;
    h2_game_runtime_drain_input(&runtime);
    assert(runtime.input_count == 0);
    return 0;
}
