#include "dinodive_internal.hpp"
#include "h2_game_test_render.hpp"

#include "math/MathUtil.h"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

namespace math = pixelroot32::math;
constexpr int kTestPlatformGap = 65;

struct Events {
    int fall = 0;
    int game_over = 0;
};

static void on_event(void *user, h2_dinodive_event_t event) {
    auto *events = static_cast<Events *>(user);
    if (event == H2_DINODIVE_EVENT_FALL) events->fall += 1;
    if (event == H2_DINODIVE_EVENT_GAME_OVER) events->game_over += 1;
}

static std::vector<unsigned char> read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

static void write_u32(std::vector<unsigned char> &data, size_t offset, uint32_t value) {
    data[offset + 0u] = static_cast<unsigned char>(value & 0xffu);
    data[offset + 1u] = static_cast<unsigned char>((value >> 8u) & 0xffu);
    data[offset + 2u] = static_cast<unsigned char>((value >> 16u) & 0xffu);
    data[offset + 3u] = static_cast<unsigned char>((value >> 24u) & 0xffu);
}

static void send_button(h2_dinodive_t *game, h2_dinodive_button_t button) {
    h2_game_input_event_t event{H2_GAME_INPUT_BUTTON_DOWN, 0, 0, static_cast<uint8_t>(button)};
    h2_dinodive_handle_input(game, &event);
}

static void press_button(h2_dinodive_t *game, h2_dinodive_button_t button) {
    h2_game_input_event_t event{H2_GAME_INPUT_BUTTON_DOWN, 0, 0, static_cast<uint8_t>(button)};
    h2_dinodive_handle_input(game, &event);
}

static void hide_hazards(h2_dinodive_t *game) {
    for (int i = 0; i < kDinoDivePlatformCount; ++i) {
        game->platforms[i].has_spike = false;
        game->hazards[i].setVisible(false);
    }
}

static void test_player_asset(const pixa_asset_t &player) {
    for (const char *name : {"run_left", "run_right"}) {
        pixa_clip_t clip{};
        assert(pixa_find_clip(&player, name, &clip) == PIXA_OK);
        assert(clip.frame_count == 4 && clip.total_duration_ms == 520 && clip.loop != 0);
        for (uint32_t index = 0; index < clip.frame_count; ++index) {
            pixa_frame_t frame{};
            assert(pixa_frame_at(&player, clip.first_frame + index, &frame) == PIXA_OK);
            assert(frame.duration_ms == 130);
        }
    }
}

static void test_physics_model(h2_dinodive_t *game) {
    assert(game->player.getBodyType() == pixelroot32::core::PhysicsBodyType::RIGID);
    assert(game->player.getHitboxOffset().x == math::toScalar(20));
    assert(game->player.getHitboxOffset().y == math::toScalar(5));
    assert(game->player.getHitboxWidth() == math::toScalar(26));
    assert(game->player.getHitboxHeight() == math::toScalar(53));
    for (int i = 0; i < kDinoDivePlatformCount; ++i) {
        assert(game->platforms[i].getBodyType() == pixelroot32::core::PhysicsBodyType::KINEMATIC);
        assert(game->platforms[i].isOneWay());
        assert(game->hazards[i].getBodyType() == pixelroot32::core::PhysicsBodyType::KINEMATIC);
        assert(game->hazards[i].isSensor());
    }

    game->scene.init();
    assert(game->player.position.y == game->platforms[0].position.y - math::toScalar(58));
}

static void test_platform_scroll_speed(h2_dinodive_t *game) {
    h2_dinodive_reset(game);
    hide_hazards(game);
    game->direction = 0;
    send_button(game, H2_DINODIVE_BUTTON_ACTION);
    const math::Scalar initial_y = game->platforms[1].position.y;
    game->scene.update(17);
    const math::Scalar expected_motion = math::toScalar(28.0f) * pixelroot32::physics::CollisionSystem::FIXED_DT;
    assert(math::abs(initial_y - game->platforms[1].position.y - expected_motion) < math::toScalar(0.01f));
}

static void assert_invalid_required_frame(const std::vector<unsigned char> &player_data,
    void (*mutate)(std::vector<unsigned char> &, const pixa_asset_t &, const pixa_clip_t &)) {
    std::vector<unsigned char> malformed_data = player_data;
    pixa_asset_t source{};
    pixa_clip_t clip{};
    assert(pixa_open_memory(malformed_data.data(), malformed_data.size(), &source) == PIXA_OK);
    assert(pixa_find_clip(&source, "run_left", &clip) == PIXA_OK);
    mutate(malformed_data, source, clip);

    pixa_asset_t malformed{};
    assert(pixa_open_memory(malformed_data.data(), malformed_data.size(), &malformed) == PIXA_OK);
    const h2_game_text_api_t text = h2_game_text_builtin_5x7();
    h2_dinodive_config_t config{
        &malformed, nullptr, &text, h2_dinodive_english_texts(), 1u, nullptr, nullptr};
    h2_dinodive_t *game = reinterpret_cast<h2_dinodive_t *>(1);
    assert(h2_dinodive_create(&config, &game) == H2_DINODIVE_ERR_ASSET);
    assert(game == nullptr);
}

static void test_required_frame_validation(const std::vector<unsigned char> &player_data) {
    assert_invalid_required_frame(player_data,
        [](std::vector<unsigned char> &data, const pixa_asset_t &asset, const pixa_clip_t &clip) {
            data[asset.frame_offset + clip.first_frame * 16u + 2u] = 0xffu;
        });
    assert_invalid_required_frame(player_data,
        [](std::vector<unsigned char> &data, const pixa_asset_t &asset, const pixa_clip_t &clip) {
            write_u32(data, asset.frame_offset + clip.first_frame * 16u + 8u, 1u);
        });
    assert_invalid_required_frame(player_data,
        [](std::vector<unsigned char> &data, const pixa_asset_t &asset, const pixa_clip_t &clip) {
            pixa_frame_t frame{};
            assert(pixa_frame_at(&asset, clip.first_frame, &frame) == PIXA_OK);
            data[asset.payload_offset + frame.payload_offset + 1u] = static_cast<unsigned char>(asset.color_count);
        });
}

static void test_terminal_fall_speed(h2_dinodive_t *game) {
    h2_dinodive_reset(game);
    hide_hazards(game);
    for (int i = 0; i < kDinoDivePlatformCount; ++i) {
        game->platforms[i].setPosition({math::toScalar(0), math::toScalar(400 + i * kTestPlatformGap)});
    }
    game->player.setPosition({math::toScalar(100), math::toScalar(0)});
    game->player.setVelocity(math::toScalar(0), math::toScalar(1000));
    game->direction = 0;
    game->was_on_floor = false;
    game->started = true;
    game->scene.update(17);
    assert(game->player.getVelocityY() == math::toScalar(150));
}

static void test_input_and_motion(h2_dinodive_t *game, Events &events) {
    h2_dinodive_reset(game);
    const math::Scalar initial_x = game->player.position.x;
    game->scene.update(34);
    assert(game->player.position.x == initial_x && events.fall == 0);

    send_button(game, H2_DINODIVE_BUTTON_ACTION);
    game->scene.update(17);
    const math::Scalar first_step = game->player.position.x - initial_x;
    assert(first_step > math::toScalar(1) && first_step < math::toScalar(2));
    assert(game->player.contacted_floor);

    press_button(game, H2_DINODIVE_BUTTON_LEFT);
    const math::Scalar before_left = game->player.position.x;
    game->scene.update(17);
    assert(game->direction == -1 && game->player.position.x < before_left);

    const int fall_before = events.fall;
    press_button(game, H2_DINODIVE_BUTTON_RIGHT);
    for (int i = 0; i < 120 && events.fall == fall_before; ++i) game->scene.update(17);
    assert(events.fall == fall_before + 1);

    h2_dinodive_reset(game);
    press_button(game, H2_DINODIVE_BUTTON_LEFT);
    assert(!game->started && game->direction == 1);
    send_button(game, H2_DINODIVE_BUTTON_ACTION);
    press_button(game, H2_DINODIVE_BUTTON_LEFT);
    game->player.setPosition({math::toScalar(-61), game->player.position.y});
    game->scene.update(17);
    assert(game->player.position.x == math::toScalar(240));
}

static void test_frame_rate_independence(const h2_dinodive_config_t &config) {
    h2_dinodive_t *small_frames = nullptr;
    h2_dinodive_t *large_frames = nullptr;
    assert(h2_dinodive_create(&config, &small_frames) == H2_DINODIVE_OK);
    assert(h2_dinodive_create(&config, &large_frames) == H2_DINODIVE_OK);
    send_button(small_frames, H2_DINODIVE_BUTTON_ACTION);
    send_button(large_frames, H2_DINODIVE_BUTTON_ACTION);

    for (int i = 0; i < 10; ++i) small_frames->scene.update(17);
    large_frames->scene.update(170);
    large_frames->scene.update(0);
    large_frames->scene.update(0);

    assert(math::abs(small_frames->player.position.x - large_frames->player.position.x) < math::toScalar(0.01f));
    assert(math::abs(small_frames->player.position.y - large_frames->player.position.y) < math::toScalar(0.01f));
    h2_dinodive_destroy(small_frames);
    h2_dinodive_destroy(large_frames);
}

static void test_landing_and_recycle(h2_dinodive_t *game) {
    h2_dinodive_reset(game);
    hide_hazards(game);
    for (int i = 0; i < kDinoDivePlatformCount; ++i) {
        game->platforms[i].setPosition({math::toScalar(0), math::toScalar(300 + i * kTestPlatformGap)});
    }
    game->platforms[0].setPosition({math::toScalar(80), math::toScalar(150)});
    game->player.setPosition({math::toScalar(90), math::toScalar(65)});
    game->player.setVelocity(math::toScalar(0), math::toScalar(90));
    game->was_on_floor = false;
    game->direction = 0;
    game->started = true;
    for (int i = 0; i < 60 && !game->player.contacted_floor; ++i) game->scene.update(17);
    assert(game->player.contacted_floor);
    const auto player_box = game->player.getHitBox();
    assert(math::abs(player_box.position.y + math::toScalar(player_box.height) - game->platforms[0].position.y) <
        math::toScalar(1));

    h2_dinodive_reset(game);
    hide_hazards(game);
    for (int i = 0; i < kDinoDivePlatformCount; ++i) {
        game->platforms[i].setPosition({math::toScalar(0), math::toScalar(300 + i * kTestPlatformGap)});
    }
    game->platforms[0].setPosition({math::toScalar(0), math::toScalar(-13)});
    game->player.setPosition({math::toScalar(100), math::toScalar(100)});
    game->direction = 0;
    game->was_on_floor = false;
    game->started = true;
    game->scene.update(17);
    assert(game->floor_count == 1 && game->platforms[0].position.y >= math::toScalar(240));
}

static void test_hazard_game_over(h2_dinodive_t *game, Events &events) {
    h2_dinodive_reset(game);
    hide_hazards(game);
    game->platforms[0].setPosition({math::toScalar(100), math::toScalar(130)});
    game->platforms[0].has_spike = true;
    game->platforms[0].spike_on_right = false;
    game->hazards[0].setPosition({math::toScalar(102), math::toScalar(121)});
    game->hazards[0].setVisible(true);
    game->player.setPosition({math::toScalar(82), math::toScalar(70)});
    game->player.setVelocity(math::toScalar(0), math::toScalar(0));
    game->direction = 0;
    game->was_on_floor = false;
    game->started = true;
    const int game_over_before = events.game_over;
    game->scene.update(17);
    h2_dinodive_result_t result{};
    assert(h2_dinodive_get_result(game, &result) == H2_DINODIVE_OK);
    assert(result.game_over == 1 && events.game_over == game_over_before + 1);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    auto player_data = read_file(argv[1]);
    pixa_asset_t player{};
    assert(pixa_open_memory(player_data.data(), player_data.size(), &player) == PIXA_OK);
    test_player_asset(player);
    test_required_frame_validation(player_data);

    Events events;
    const h2_game_text_api_t text = h2_game_text_builtin_5x7();
    h2_dinodive_config_t config{
        &player, nullptr, &text, h2_dinodive_english_texts(),
        0x12345678u, on_event, &events};
    h2_dinodive_config_t invalid_config{};
    h2_dinodive_t *invalid_game = reinterpret_cast<h2_dinodive_t *>(1);
    assert(h2_dinodive_create(&invalid_config, &invalid_game) == H2_DINODIVE_ERR_INVALID_ARG);
    assert(invalid_game == nullptr);
    h2_dinodive_t *game = nullptr;
    assert(h2_dinodive_create(&config, &game) == H2_DINODIVE_OK && game != nullptr);
    test_physics_model(game);
    test_platform_scroll_speed(game);
    test_terminal_fall_speed(game);
    test_input_and_motion(game, events);
    test_frame_rate_independence(config);
    test_landing_and_recycle(game);
    test_hazard_game_over(game, events);

    h2_dinodive_reset(game);
    const math::Scalar deterministic_x = game->platforms[0].position.x;
    h2_dinodive_reset(game);
    assert(game->direction == 1 && game->floor_count == 0 && !game->started && !game->game_over);
    assert(game->platforms[0].position.x == deterministic_x);

    const h2_dinodive_texts_t translated = {
        H2_GAME_TEXT_LITERAL("层数"),
        H2_GAME_TEXT_LITERAL("按录音键开始"),
    };
    const h2_game_text_span_t expected[] = {translated.floor, translated.press_start};
    bool drawn[2]{};
    H2GameMockTextState text_state{expected, drawn, 2u};
    const h2_game_text_api_t translated_text = h2_game_test_text_api(&text_state);
    game->config.text = &translated_text;
    game->config.texts = &translated;
    auto renderer = h2_game_test_renderer();
    game->scene.draw(renderer);
    assert(drawn[0] && drawn[1]);
    h2_dinodive_destroy(game);
    return 0;
}
