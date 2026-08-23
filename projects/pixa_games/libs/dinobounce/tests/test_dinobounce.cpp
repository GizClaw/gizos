#include "dinobounce_internal.hpp"
#include "h2_game_test_render.hpp"

#include <cassert>

struct Events { int bounce = 0; int game_over = 0; };
static void on_event(void *user, h2_dinobounce_event_t event) {
    auto *events = static_cast<Events *>(user);
    if (event == H2_DINOBOUNCE_EVENT_BOUNCE) ++events->bounce;
    else ++events->game_over;
}
static void click(h2_dinobounce *game, uint8_t button) {
    const h2_game_input_event_t event = {H2_GAME_INPUT_BUTTON_CLICK, 0, 0, button};
    h2_dinobounce_handle_input(game, &event);
}

int main() {
    Events events;
    const h2_game_text_api_t text = h2_game_text_builtin_5x7();
    const h2_dinobounce_config_t config = {
        nullptr, nullptr, &text, h2_dinobounce_english_texts(),
        0x12345678u, on_event, &events};
    h2_dinobounce game(config);
    game.scene.init();
    assert(game.paddle_x == 90 && !game.launched && !game.game_over);
    click(&game, H2_DINOBOUNCE_BUTTON_LEFT);
    game.scene.update(17);
    assert(game.paddle_x == 86 && game.paddle_dir == -1);
    click(&game, H2_DINOBOUNCE_BUTTON_RIGHT);
    game.scene.update(17);
    assert(game.paddle_x == 90 && game.paddle_dir == 1);
    for (int i = 0; i < 480; ++i) game.scene.update(17);
    assert(game.level >= 1 && game.ball_speed >= 305);
    click(&game, H2_DINOBOUNCE_BUTTON_ACTION);
    assert(game.launched && game.ball_vy == -game.ball_speed);
    game.game_over = true;
    click(&game, H2_DINOBOUNCE_BUTTON_ACTION);
    assert(!game.game_over && game.launched && game.paddle_x == 90);
    h2_dinobounce_result_t result{};
    assert(h2_dinobounce_get_result(&game, &result) == H2_DINOBOUNCE_OK);

    const h2_dinobounce_texts_t translated = {
        H2_GAME_TEXT_LITERAL("按录音键开始"),
        H2_GAME_TEXT_LITERAL("游戏结束"),
    };
    const h2_game_text_span_t expected[] = {translated.press_record, translated.game_over};
    bool drawn[2]{};
    H2GameMockTextState text_state{expected, drawn, 2u};
    const h2_game_text_api_t translated_text = h2_game_test_text_api(&text_state);
    game.config.text = &translated_text;
    game.config.texts = &translated;
    game.launched = false;
    game.game_over = true;
    auto renderer = h2_game_test_renderer();
    game.scene.draw(renderer);
    assert(drawn[0] && drawn[1]);
    return 0;
}
