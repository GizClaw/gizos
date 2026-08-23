#include "dinobounce_internal.hpp"

#include "graphics/Color.h"
#include "graphics/Renderer.h"

#include <algorithm>
#include <cstdlib>
#include <new>

namespace {
constexpr int kFrameUs = 16667;
constexpr int kScreen = 240;
constexpr int kScale = 100;
constexpr int kPaddleY = 185;
constexpr int kPaddleW = 60;
constexpr int kPaddleH = 12;
constexpr int kPaddleSpeed = 4;
constexpr int kBallSpeedBase = 280;
constexpr int kBallSpeedIncrement = 25;
constexpr int kBallSpeedMax = 450;
constexpr int kBallW = 16;
constexpr int kBallH = 21;
constexpr int kBrickW = 40;
constexpr int kBrickH = 12;
constexpr int kBrickStep = 16;

const h2_dinobounce_texts_t kEnglishTexts = {
    H2_GAME_TEXT_LITERAL("PRESS RECORD"),
    H2_GAME_TEXT_LITERAL("GAME OVER"),
};

bool valid_text(h2_game_text_span_t text) {
    return text.data != nullptr && text.byte_len != 0u &&
           h2_game_text_validate_utf8(text) == H2_GAME_TEXT_OK;
}

void draw_centered_text(
    const h2_dinobounce *game,
    pixelroot32::graphics::Renderer &renderer,
    h2_game_text_span_t text,
    int y,
    uint16_t line_height) {
    h2_game_text_metrics_t metrics{};
    if (h2_game_text_measure(game->config.text, text, line_height, &metrics) !=
        H2_GAME_TEXT_OK) return;
    const h2_game_text_surface_t surface{
        renderer.getDrawSurface().getPixelBuffer(), kScreen, kScreen, kScreen,
        static_cast<size_t>(kScreen * kScreen)};
    const uint16_t color = pixelroot32::graphics::resolveColor(
        pixelroot32::graphics::Color::White,
        pixelroot32::graphics::PaletteContext::Sprite);
    (void)h2_game_text_draw(
        game->config.text, &surface, text, (kScreen - metrics.width_px) / 2, y,
        {color, line_height});
}
const uint8_t kFireballArgb4444[] = {
#include "dinobounce_fireball_argb4444.inc"
};
static_assert(sizeof(kFireballArgb4444) == kBallW * kBallH * 2);

uint32_t random_next(h2_dinobounce *game) {
    uint32_t value = game->random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    game->random_state = value == 0 ? 1 : value;
    return game->random_state;
}

bool clip_valid(const pixa_asset_t *asset, const char *name) {
    pixa_clip_t clip{};
    return pixa_find_clip(asset, name, &clip) == PIXA_OK &&
           clip.frame_count > 0;
}

bool overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

void emit(h2_dinobounce *game, h2_dinobounce_event_t event) {
    if (game->config.event_callback != nullptr) {
        game->config.event_callback(game->config.event_user, event);
    }
}

void draw_player(h2_dinobounce *game, pixelroot32::graphics::Renderer &renderer) {
    if (game->config.player == nullptr || !game->player_bgra || !game->player_argb4444) return;
    pixa_clip_t clip{};
    uint32_t frame = 0;
    if (pixa_find_clip(game->config.player, game->clip_name, &clip) != PIXA_OK ||
        pixa_frame_index_at_ms(game->config.player, &clip, game->elapsed_us / 1000u, &frame) != PIXA_OK) return;
    if (game->decoded_clip != game->clip_name || game->decoded_frame != frame) {
        const size_t bgra_size = pixa_canvas_bgra_bytes(game->config.player->canvas);
        const size_t argb_size = pixa_canvas_argb4444_bytes(game->config.player->canvas);
        if (pixa_decode_clip_frame_bgra(game->config.player, game->clip_name, frame, game->player_bgra.get(), bgra_size) != PIXA_OK ||
            pixa_bgra_to_argb4444(game->player_bgra.get(), bgra_size, game->player_argb4444.get(), argb_size) != PIXA_OK) return;
        game->decoded_clip = game->clip_name;
        game->decoded_frame = frame;
    }
    (void)pixa_blit_argb4444_to_rgb565(
        renderer.getDrawSurface().getPixelBuffer(), kScreen, kScreen, kScreen,
        game->player_argb4444.get(), game->config.player->canvas.width,
        game->config.player->canvas.height, static_cast<int16_t>(game->paddle_x), 190);
}

void spawn_bricks(h2_dinobounce *game) {
    for (auto &brick : game->bricks) brick = {};
    size_t index = 0;
    for (int row = 0; row < 4; ++row) {
        const int count = 2 + static_cast<int>(random_next(game) % 3u);
        bool used[5]{};
        for (int n = 0; n < count && index < game->bricks.size(); ++n) {
            int slot = static_cast<int>(random_next(game) % 5u);
            while (used[slot]) slot = (slot + 1) % 5;
            used[slot] = true;
            game->bricks[index++] = {
                static_cast<int16_t>(4 + slot * 44),
                static_cast<int16_t>(10 + row * 16),
                static_cast<uint8_t>(row), true};
        }
    }
}

void spawn_top_row(h2_dinobounce *game) {
    bool used[5]{};
    const int count = 2 + static_cast<int>(random_next(game) % 3u);
    for (int n = 0; n < count; ++n) {
        auto free_brick = std::find_if(game->bricks.begin(), game->bricks.end(),
                                       [](const auto &brick) { return !brick.alive; });
        if (free_brick == game->bricks.end()) return;
        int slot = static_cast<int>(random_next(game) % 5u);
        while (used[slot]) slot = (slot + 1) % 5;
        used[slot] = true;
        *free_brick = {static_cast<int16_t>(4 + slot * 44), 10,
                       static_cast<uint8_t>(random_next(game) % 4u), true};
    }
}

void reset(h2_dinobounce *game) {
    game->random_state = game->config.random_seed == 0 ? 1 : game->config.random_seed;
    game->accumulator_us = 0;
    game->elapsed_us = 0;
    game->paddle_x = 90;
    game->paddle_dir = 0;
    game->ball_x = (game->paddle_x + kPaddleW / 2 - kBallW / 2) * kScale;
    game->ball_y = (kPaddleY - kBallH) * kScale;
    game->ball_vx = game->ball_vy = 0;
    game->ball_speed = kBallSpeedBase;
    game->level = 0;
    game->descend_frames = 0;
    game->launched = false;
    game->game_over = false;
    game->clip_name = "run_right";
    game->decoded_frame = UINT32_MAX;
    game->decoded_clip = nullptr;
    spawn_bricks(game);
}

void finish(h2_dinobounce *game) {
    if (!game->game_over) {
        game->game_over = true;
        emit(game, H2_DINOBOUNCE_EVENT_GAME_OVER);
    }
}
} // namespace

const h2_dinobounce_texts_t *h2_dinobounce_english_texts(void) {
    return &kEnglishTexts;
}

void DinoBounceScene::init() {
    pixelroot32::core::Scene::init();
    reset(owner);
}

void DinoBounceScene::update(unsigned long delta_ms) {
    if (owner->game_over) return;
    owner->accumulator_us += static_cast<uint64_t>(delta_ms) * 1000u;
    uint8_t steps = 0;
    while (!owner->game_over && owner->accumulator_us >= kFrameUs &&
           steps++ < 8) {
        step();
        owner->accumulator_us -= kFrameUs;
    }
}

void DinoBounceScene::step() {
    owner->elapsed_us += kFrameUs;
    owner->paddle_x = std::clamp<int32_t>(owner->paddle_x + owner->paddle_dir * kPaddleSpeed, 0, kScreen - kPaddleW);
    if (!owner->launched) {
        owner->ball_x = (owner->paddle_x + kPaddleW / 2 - kBallW / 2) * kScale;
        owner->ball_y = (kPaddleY - kBallH) * kScale;
    } else {
        owner->ball_x += owner->ball_vx;
        owner->ball_y += owner->ball_vy;
        int x = owner->ball_x / kScale;
        int y = owner->ball_y / kScale;
        if (x <= 0) { owner->ball_x = 0; owner->ball_vx = std::abs(owner->ball_vx); }
        if (x + kBallW >= kScreen) { owner->ball_x = (kScreen - kBallW) * kScale; owner->ball_vx = -std::abs(owner->ball_vx); }
        if (y <= 0) { owner->ball_y = 0; owner->ball_vy = std::abs(owner->ball_vy); }
        x = owner->ball_x / kScale;
        y = owner->ball_y / kScale;
        if (owner->ball_vy > 0 && overlap(x, y, kBallW, kBallH, owner->paddle_x, kPaddleY, kPaddleW, kPaddleH)) {
            owner->ball_y = (kPaddleY - kBallH) * kScale;
            owner->ball_vy = -std::abs(owner->ball_vy);
            const int hit = x + kBallW / 2 - (owner->paddle_x + kPaddleW / 2);
            owner->ball_vx = std::clamp<int32_t>(owner->ball_vx + hit * owner->ball_speed / kPaddleW + owner->paddle_dir * 30, -owner->ball_speed, owner->ball_speed);
            emit(owner, H2_DINOBOUNCE_EVENT_BOUNCE);
        }
        for (auto &brick : owner->bricks) {
            if (!brick.alive || !overlap(x, y, kBallW, kBallH, brick.x, brick.y, kBrickW, kBrickH)) continue;
            const int dx = x + kBallW / 2 - (brick.x + kBrickW / 2);
            const int dy = y + kBallH / 2 - (brick.y + kBrickH / 2);
            if (std::abs(dx) * kBrickH > std::abs(dy) * kBrickW) owner->ball_vx = -owner->ball_vx;
            else owner->ball_vy = -owner->ball_vy;
            brick.alive = false;
            break;
        }
        if (y > kScreen) finish(owner);
    }
    const int next_level = static_cast<int>(owner->elapsed_us / 8000000u);
    if (next_level != owner->level) {
        const int old = owner->ball_speed;
        owner->level = next_level;
        owner->ball_speed = std::min<int32_t>(
            kBallSpeedBase + owner->level * kBallSpeedIncrement,
            kBallSpeedMax);
        if (owner->launched && old > 0) {
            owner->ball_vx = owner->ball_vx * owner->ball_speed / old;
            owner->ball_vy = owner->ball_vy * owner->ball_speed / old;
        }
    }
    const int interval = std::max<int32_t>(300 - owner->level * 30, 90);
    if (++owner->descend_frames >= interval) {
        owner->descend_frames = 0;
        for (auto &brick : owner->bricks) if (brick.alive) {
            brick.y += kBrickStep;
            if (brick.y + kBrickH >= kPaddleY) finish(owner);
        }
        if (!owner->game_over) spawn_top_row(owner);
    }
}

void DinoBounceScene::draw(pixelroot32::graphics::Renderer &renderer) {
    renderer.drawFilledRectangleW(0, 0, kScreen, kScreen, 0x0841);
    static const uint16_t colors[] = {0xf81f, 0x07ff, 0xffe0, 0xf800};
    for (const auto &brick : owner->bricks) if (brick.alive)
        renderer.drawFilledRectangleW(brick.x, brick.y, kBrickW, kBrickH, colors[brick.color % 4]);
    draw_player(owner, renderer);
    // Match the legacy LVGL object order: the player is created first and the
    // paddle is created above it, so the tray stays visibly in the dino's hands.
    renderer.drawFilledRectangleW(owner->paddle_x, kPaddleY, kPaddleW, kPaddleH, 0x07ff);
    const int bx = owner->ball_x / kScale;
    const int by = owner->ball_y / kScale;
    (void)pixa_blit_argb4444_to_rgb565(
        renderer.getDrawSurface().getPixelBuffer(), kScreen, kScreen, kScreen,
        kFireballArgb4444, kBallW, kBallH, static_cast<int16_t>(bx),
        static_cast<int16_t>(by));
    if (!owner->launched) {
        draw_centered_text(owner, renderer, owner->config.texts->press_record, 210, 8);
    }
    if (owner->game_over) {
        draw_centered_text(owner, renderer, owner->config.texts->game_over, 105, 16);
    }
}

int h2_dinobounce_create(const h2_dinobounce_config_t *config, h2_dinobounce_t **out_game) {
    if (out_game == nullptr) return H2_DINOBOUNCE_ERR_INVALID_ARGUMENT;
    *out_game = nullptr;
    if (config == nullptr || config->player == nullptr || config->text == nullptr ||
        config->text->vtable == nullptr || config->text->vtable->measure == nullptr ||
        config->text->vtable->draw == nullptr || config->texts == nullptr ||
        !valid_text(config->texts->press_record) || !valid_text(config->texts->game_over)) {
        return H2_DINOBOUNCE_ERR_INVALID_ARGUMENT;
    }
    if (!clip_valid(config->player, "run_left") || !clip_valid(config->player, "run_right")) return H2_DINOBOUNCE_ERR_ASSET;
    auto *game = new (std::nothrow) h2_dinobounce(*config);
    if (game == nullptr) return H2_DINOBOUNCE_ERR_NO_MEMORY;
    const size_t bgra_size = pixa_canvas_bgra_bytes(config->player->canvas);
    const size_t argb_size = pixa_canvas_argb4444_bytes(config->player->canvas);
    game->player_bgra.reset(new (std::nothrow) uint8_t[bgra_size]);
    game->player_argb4444.reset(new (std::nothrow) uint8_t[argb_size]);
    if (!game->player_bgra || !game->player_argb4444) { delete game; return H2_DINOBOUNCE_ERR_NO_MEMORY; }
    game->scene.init();
    *out_game = game;
    return H2_DINOBOUNCE_OK;
}

h2_game_scene_t *h2_dinobounce_scene(h2_dinobounce_t *game) {
    return game == nullptr ? nullptr : reinterpret_cast<h2_game_scene_t *>(&game->scene);
}

void h2_dinobounce_handle_input(h2_dinobounce_t *game, const h2_game_input_event_t *event) {
    if (game == nullptr || event == nullptr ||
        (event->type != H2_GAME_INPUT_BUTTON_DOWN && event->type != H2_GAME_INPUT_BUTTON_CLICK)) return;
    if (game->game_over) {
        if (event->button != H2_DINOBOUNCE_BUTTON_ACTION) return;
        reset(game);
    }
    if (event->button == H2_DINOBOUNCE_BUTTON_LEFT) { game->paddle_dir = -1; game->clip_name = "run_left"; }
    else if (event->button == H2_DINOBOUNCE_BUTTON_RIGHT) { game->paddle_dir = 1; game->clip_name = "run_right"; }
    else if (event->button == H2_DINOBOUNCE_BUTTON_ACTION && !game->launched) {
        game->launched = true;
        const int angle = static_cast<int>(random_next(game) % 91u) - 45;
        game->ball_vx = angle * game->ball_speed / 60;
        game->ball_vy = -game->ball_speed;
    }
}

void h2_dinobounce_reset(h2_dinobounce_t *game) { if (game != nullptr) reset(game); }
int h2_dinobounce_get_result(const h2_dinobounce_t *game, h2_dinobounce_result_t *out_result) {
    if (game == nullptr || out_result == nullptr) return H2_DINOBOUNCE_ERR_INVALID_ARGUMENT;
    *out_result = {static_cast<uint32_t>(game->elapsed_us / 1000u), static_cast<uint8_t>(game->game_over)};
    return H2_DINOBOUNCE_OK;
}
void h2_dinobounce_destroy(h2_dinobounce_t *game) {
    if (game == nullptr) return;
    delete game;
}
