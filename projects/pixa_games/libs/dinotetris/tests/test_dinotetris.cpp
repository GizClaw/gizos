#include "dinotetris_internal.hpp"
#include "h2_game_test_render.hpp"

#include <cassert>

struct Events {
    int rotate = 0;
    int fast_drop = 0;
    int locked = 0;
    int game_over = 0;
};

static void on_event(void *user, h2_dinotetris_event_t event) {
    auto *events = static_cast<Events *>(user);
    if (event == H2_DINOTETRIS_EVENT_ROTATE) ++events->rotate;
    else if (event == H2_DINOTETRIS_EVENT_FAST_DROP) ++events->fast_drop;
    else if (event == H2_DINOTETRIS_EVENT_PIECE_LOCKED) ++events->locked;
    else if (event == H2_DINOTETRIS_EVENT_GAME_OVER) ++events->game_over;
}

static void input(h2_dinotetris *game, h2_game_input_type_t type, uint8_t button) {
    const h2_game_input_event_t event = {type, 0, 0, button};
    h2_dinotetris_handle_input(game, &event);
}

int main() {
    Events events;
    const h2_game_text_api_t text = h2_game_text_builtin_5x7();
    const h2_dinotetris_config_t config = {
        &text, h2_dinotetris_english_texts(), 0x12345678u, on_event, &events};
    h2_dinotetris_t *game = nullptr;
    assert(h2_dinotetris_create(&config, &game) == H2_DINOTETRIS_OK);
    const uint32_t initial_random_state = game->random_state;
    const auto initial_current = game->current;
    const uint8_t initial_next_shape = game->next_shape;
    game->scene.init();
    assert(game->random_state == initial_random_state);
    assert(game->current.type == initial_current.type &&
           game->current.x == initial_current.x &&
           game->current.y == initial_current.y &&
           game->current.rotation == initial_current.rotation);
    assert(game->next_shape == initial_next_shape);
    assert(game != nullptr && game->level == 1 && game->current.x == 3);
    const uint32_t random_state_after_create = game->random_state;
    h2_dinotetris_reset(game);
    assert(game->random_state != random_state_after_create);

    input(game, H2_GAME_INPUT_BUTTON_CLICK, H2_DINOTETRIS_BUTTON_LEFT);
    assert(game->current.x == 2);
    input(game, H2_GAME_INPUT_BUTTON_CLICK, H2_DINOTETRIS_BUTTON_RIGHT);
    assert(game->current.x == 3);

    input(game, H2_GAME_INPUT_BUTTON_DOWN, H2_DINOTETRIS_BUTTON_LEFT);
    assert(game->current.x == 2);
    for (int i = 0; i < 9; ++i) game->scene.update(16);
    assert(game->current.x == 2);
    game->scene.update(16);
    assert(game->current.x == 1);
    for (int i = 0; i < 5; ++i) game->scene.update(16);
    assert(game->current.x == 0);
    input(game, H2_GAME_INPUT_BUTTON_UP, H2_DINOTETRIS_BUTTON_LEFT);
    for (int i = 0; i < 20; ++i) game->scene.update(16);
    assert(game->current.x == 0);
    input(game, H2_GAME_INPUT_BUTTON_CLICK, H2_DINOTETRIS_BUTTON_RIGHT);
    input(game, H2_GAME_INPUT_BUTTON_CLICK, H2_DINOTETRIS_BUTTON_RIGHT);
    input(game, H2_GAME_INPUT_BUTTON_CLICK, H2_DINOTETRIS_BUTTON_RIGHT);
    assert(game->current.x == 3);

    const uint8_t old_rotation = game->current.rotation;
    input(game, H2_GAME_INPUT_BUTTON_DOWN, H2_DINOTETRIS_BUTTON_ACTION);
    game->scene.update(80);
    input(game, H2_GAME_INPUT_BUTTON_UP, H2_DINOTETRIS_BUTTON_ACTION);
    assert(game->current.rotation != old_rotation && events.rotate == 1);

    input(game, H2_GAME_INPUT_BUTTON_DOWN, H2_DINOTETRIS_BUTTON_ACTION);
    for (int i = 0; i < 19; ++i) game->scene.update(16);
    assert(!game->fast_drop && events.fast_drop == 0);
    game->scene.update(16);
    assert(game->fast_drop && events.fast_drop == 1);
    const int old_y = game->current.y;
    game->scene.update(32);
    assert(game->current.y > old_y);
    input(game, H2_GAME_INPUT_BUTTON_UP, H2_DINOTETRIS_BUTTON_ACTION);
    assert(!game->fast_drop && events.rotate == 1);

    game->current = {1, 3, 18, 0};
    game->drop_frames = 0;
    game->lock_frames = 0;
    const int locked_before_delay = events.locked;
    for (int i = 0; i < 29; ++i) game->scene.update(16);
    assert(events.locked == locked_before_delay);
    input(game, H2_GAME_INPUT_BUTTON_DOWN, H2_DINOTETRIS_BUTTON_LEFT);
    assert(game->current.x == 2 && game->lock_frames == 0);
    input(game, H2_GAME_INPUT_BUTTON_UP, H2_DINOTETRIS_BUTTON_LEFT);
    for (int i = 0; i < 29; ++i) game->scene.update(16);
    assert(events.locked == locked_before_delay);
    game->scene.update(16);
    assert(events.locked == locked_before_delay + 1);

    game->game_over = true;
    game->game_over_animation_ms = 300;
    input(game, H2_GAME_INPUT_BUTTON_DOWN, H2_DINOTETRIS_BUTTON_ACTION);
    assert(game->game_over && game->restart_requested);
    game->scene.update(299);
    assert(game->game_over);
    game->scene.update(1);
    assert(!game->game_over && game->score == 0 && game->level == 1);
    h2_dinotetris_result_t result{};
    assert(h2_dinotetris_get_result(game, &result) == H2_DINOTETRIS_OK);

    const h2_dinotetris_texts_t translated = {
        H2_GAME_TEXT_LITERAL("分数"),
        H2_GAME_TEXT_LITERAL("等级"),
        H2_GAME_TEXT_LITERAL("下一个"),
        H2_GAME_TEXT_LITERAL("游戏结束"),
        H2_GAME_TEXT_LITERAL("经验"),
    };
    const h2_game_text_span_t expected[] = {
        translated.score, translated.level, translated.next,
        translated.game_over, translated.experience};
    bool drawn[5]{};
    H2GameMockTextState text_state{expected, drawn, 5u};
    const h2_game_text_api_t translated_text = h2_game_test_text_api(&text_state);
    game->config.text = &translated_text;
    game->config.texts = &translated;
    game->game_over = false;
    auto renderer = h2_game_test_renderer();
    game->scene.draw(renderer);
    game->game_over = true;
    game->game_over_animation_ms = 300;
    game->scene.draw(renderer);
    assert(drawn[0] && drawn[1] && drawn[2] && drawn[3] && drawn[4]);
    h2_dinotetris_destroy(game);
    return 0;
}
