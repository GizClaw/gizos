#include "dinotetris_internal.hpp"

#include "graphics/Color.h"
#include "graphics/Renderer.h"
#include "pixa.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace {
constexpr uint32_t kFrameUs = 16000u;
constexpr int kRows = 20;
constexpr int kCols = 10;
constexpr int kCell = 11;
constexpr int kBoardX = 10;
constexpr int kBoardY = 10;
constexpr int kScreen = 240;
constexpr int kInfoX = 128;
constexpr int kInfoWidth = 108;
constexpr int kPreviewX = 162;
constexpr int kPreviewY = 144;
constexpr int kDropInitial = 18;
constexpr int kDropMin = 2;
constexpr int kDropFast = 1;
constexpr int kDropDecrease = 3;
constexpr int kLongPressFrames = 20;
constexpr int kLockDelayFrames = 30;
constexpr int kHorizontalRepeatDelayFrames = 10;
constexpr int kHorizontalRepeatFrames = 5;
constexpr uint32_t kPopupAnimationMs = 300u;

const h2_dinotetris_texts_t kEnglishTexts = {
    H2_GAME_TEXT_LITERAL("SCORE"),
    H2_GAME_TEXT_LITERAL("LEVEL"),
    H2_GAME_TEXT_LITERAL("NEXT"),
    H2_GAME_TEXT_LITERAL("GAME OVER"),
    H2_GAME_TEXT_LITERAL("EXP"),
};
const h2_game_text_span_t kColonSpace H2_GAME_TEXT_LITERAL(": ");
const h2_game_text_span_t kSpacePlus H2_GAME_TEXT_LITERAL(" +");

bool valid_text(h2_game_text_span_t text) {
    return text.data != nullptr && text.byte_len != 0u &&
           h2_game_text_validate_utf8(text) == H2_GAME_TEXT_OK;
}

constexpr uint16_t kShapes[7][4] = {
    {0x0f00, 0x2222, 0x00f0, 0x4444},
    {0x6600, 0x6600, 0x6600, 0x6600},
    {0x0e40, 0x4c40, 0x4e00, 0x4640},
    {0x06c0, 0x8c40, 0x6c00, 0x4620},
    {0x0c60, 0x4c80, 0xc600, 0x2640},
    {0x44c0, 0x8e00, 0x6440, 0x0e20},
    {0x4460, 0x0e80, 0xc440, 0x2e00},
};

constexpr uint16_t kColors[7] = {
    0x07ff, 0xffe0, 0x8010, 0x07e0, 0xf800, 0x001f, 0xfd20,
};

constexpr uint8_t kBackgroundArgb4444[] = {
#include "dinotetris_background_argb4444.inc"
};

static_assert(sizeof(kBackgroundArgb4444) == kScreen * kScreen * 2u);

void blit_background(pixelroot32::graphics::Renderer &renderer) {
    (void)pixa_blit_argb4444_to_rgb565(
        renderer.getDrawSurface().getPixelBuffer(), kScreen, kScreen, kScreen,
        kBackgroundArgb4444, kScreen, kScreen, 0, 0);
}

int measure_text(const h2_dinotetris *game,
                 h2_game_text_span_t text,
                 uint16_t line_height) {
    h2_game_text_metrics_t metrics{};
    return h2_game_text_measure(game->config.text, text, line_height, &metrics) ==
            H2_GAME_TEXT_OK
        ? metrics.width_px
        : -1;
}

int text_advance(const h2_dinotetris *game,
                 h2_game_text_span_t text,
                 uint16_t line_height) {
    h2_game_text_metrics_t metrics{};
    return h2_game_text_measure(game->config.text, text, line_height, &metrics) ==
            H2_GAME_TEXT_OK
        ? metrics.advance_px
        : -1;
}

void draw_text(const h2_dinotetris *game,
               pixelroot32::graphics::Renderer &renderer,
               h2_game_text_span_t text,
               int x,
               int y,
               pixelroot32::graphics::Color color,
               uint16_t line_height) {
    const h2_game_text_surface_t surface{
        renderer.getDrawSurface().getPixelBuffer(), kScreen, kScreen, kScreen,
        static_cast<size_t>(kScreen * kScreen)};
    const uint16_t resolved = pixelroot32::graphics::resolveColor(
        color, pixelroot32::graphics::PaletteContext::Sprite);
    (void)h2_game_text_draw(
        game->config.text, &surface, text, x, y, {resolved, line_height});
}

void draw_info_text(const h2_dinotetris *game,
                    pixelroot32::graphics::Renderer &renderer,
                    h2_game_text_span_t text,
                    int y,
                    pixelroot32::graphics::Color color,
                    uint16_t line_height) {
    const int width = measure_text(game, text, line_height);
    if (width >= 0) {
        draw_text(game, renderer, text, kInfoX + (kInfoWidth - width) / 2, y,
                  color, line_height);
    }
}

void draw_centered_text(const h2_dinotetris *game,
                        pixelroot32::graphics::Renderer &renderer,
                        h2_game_text_span_t text,
                        int y,
                        pixelroot32::graphics::Color color,
                        uint16_t line_height) {
    const int width = measure_text(game, text, line_height);
    if (width >= 0) {
        draw_text(game, renderer, text, (kScreen - width) / 2, y, color, line_height);
    }
}

void draw_centered_three_parts(
    const h2_dinotetris *game,
    pixelroot32::graphics::Renderer &renderer,
    h2_game_text_span_t first,
    h2_game_text_span_t middle,
    h2_game_text_span_t last,
    int y,
    uint16_t line_height) {
    const int first_advance = text_advance(game, first, line_height);
    const int middle_advance = text_advance(game, middle, line_height);
    const int last_width = measure_text(game, last, line_height);
    const int64_t total_width = static_cast<int64_t>(first_advance) + middle_advance + last_width;
    if (first_advance < 0 || middle_advance < 0 || last_width < 0 ||
        total_width > INT32_MAX) return;
    int x = static_cast<int>((kScreen - total_width) / 2);
    draw_text(game, renderer, first, x, y, pixelroot32::graphics::Color::White, line_height);
    x += first_advance;
    draw_text(game, renderer, middle, x, y, pixelroot32::graphics::Color::White, line_height);
    x += middle_advance;
    draw_text(game, renderer, last, x, y, pixelroot32::graphics::Color::White, line_height);
}

void darken_for_game_over(pixelroot32::graphics::Renderer &renderer, int top) {
    uint16_t *pixels = renderer.getDrawSurface().getPixelBuffer();
    constexpr uint32_t remaining = 105u;
    for (int index = top * kScreen; index < kScreen * kScreen; ++index) {
        const uint16_t color = pixels[index];
        const uint16_t red = static_cast<uint16_t>(((color >> 11u) & 0x1fu) * remaining / 255u);
        const uint16_t green = static_cast<uint16_t>(((color >> 5u) & 0x3fu) * remaining / 255u);
        const uint16_t blue = static_cast<uint16_t>((color & 0x1fu) * remaining / 255u);
        pixels[index] = static_cast<uint16_t>((red << 11u) | (green << 5u) | blue);
    }
}

int game_over_popup_offset(const h2_dinotetris *game) {
    if (game->restart_requested) {
        const uint32_t elapsed = std::min(game->restart_animation_ms, kPopupAnimationMs);
        const uint64_t cubic = static_cast<uint64_t>(elapsed) * elapsed * elapsed;
        const uint64_t duration_cubic = static_cast<uint64_t>(kPopupAnimationMs) *
                                        kPopupAnimationMs * kPopupAnimationMs;
        return static_cast<int>(kScreen * cubic / duration_cubic);
    }
    const uint32_t elapsed = std::min(game->game_over_animation_ms, kPopupAnimationMs);
    const uint32_t remaining = kPopupAnimationMs - elapsed;
    const uint64_t cubic = static_cast<uint64_t>(remaining) * remaining * remaining;
    const uint64_t duration_cubic = static_cast<uint64_t>(kPopupAnimationMs) *
                                    kPopupAnimationMs * kPopupAnimationMs;
    return static_cast<int>(kScreen * cubic / duration_cubic);
}

uint32_t random_next(h2_dinotetris *game) {
    uint32_t value = game->random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    game->random_state = value == 0 ? 1 : value;
    return game->random_state;
}

bool shape_cell(uint8_t type, uint8_t rotation, int row, int col) {
    const int bit = (3 - row) * 4 + (3 - col);
    return ((kShapes[type % 7][rotation % 4] >> bit) & 1u) != 0;
}

void emit(h2_dinotetris *game, h2_dinotetris_event_t event) {
    if (game->config.event_callback != nullptr)
        game->config.event_callback(game->config.event_user, event);
}

int drop_interval(const h2_dinotetris *game) {
    return std::max(kDropInitial - static_cast<int>(game->level - 1) * kDropDecrease,
                    kDropMin);
}

bool collision(const h2_dinotetris *game, int x, int y, int rotation) {
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) {
        if (!shape_cell(game->current.type, static_cast<uint8_t>(rotation), row,
                        col)) continue;
        const int board_row = y + row;
        const int board_col = x + col;
        if (board_col < 0 || board_col >= kCols || board_row >= kRows) return true;
        if (board_row >= 0 && game->board[board_row][board_col] != 0) return true;
    }
    return false;
}

void spawn(h2_dinotetris *game) {
    game->current = {game->next_shape, 3, 0, 0};
    game->drop_frames = 0;
    game->lock_frames = 0;
    game->next_shape = static_cast<uint8_t>(random_next(game) % 7u);
    if (collision(game, game->current.x, game->current.y, 0)) {
        game->game_over = true;
        game->game_over_animation_ms = 0;
        game->restart_animation_ms = 0;
        game->restart_requested = false;
        game->fast_drop = false;
        game->action_pressed = false;
        emit(game, H2_DINOTETRIS_EVENT_GAME_OVER);
    }
}

void clear_lines(h2_dinotetris *game) {
    int cleared = 0;
    for (int row = kRows - 1; row >= 0; --row) {
        bool full = true;
        for (int col = 0; col < kCols; ++col) full &= game->board[row][col] != 0;
        if (!full) continue;
        ++cleared;
        for (int y = row; y > 0; --y) game->board[y] = game->board[y - 1];
        game->board[0].fill(0);
        ++row;
    }
    if (cleared == 0) return;
    constexpr uint32_t scores[] = {0, 100, 300, 500, 800};
    game->score += scores[std::min(cleared, 4)] * game->level;
    game->lines_cleared += static_cast<uint32_t>(cleared);
    game->level = game->lines_cleared / 5u + 1u;
}

void lock_piece(h2_dinotetris *game) {
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) {
        if (!shape_cell(game->current.type, game->current.rotation, row, col)) continue;
        const int y = game->current.y + row;
        const int x = game->current.x + col;
        if (y >= 0 && y < kRows && x >= 0 && x < kCols)
            game->board[y][x] = static_cast<uint8_t>(game->current.type + 1);
    }
    clear_lines(game);
    emit(game, H2_DINOTETRIS_EVENT_PIECE_LOCKED);
    spawn(game);
}

bool move_down(h2_dinotetris *game) {
    if (!collision(game, game->current.x, game->current.y + 1,
                   game->current.rotation)) {
        ++game->current.y;
        return true;
    }
    return false;
}

bool rotate(h2_dinotetris *game) {
    const uint8_t rotation = static_cast<uint8_t>((game->current.rotation + 1) % 4);
    constexpr int kicks[] = {0, -1, 1, -2, 2};
    for (int kick : kicks) {
        const int x = game->current.x + kick;
        if (collision(game, x, game->current.y, rotation)) continue;
        game->current.x = static_cast<int8_t>(x);
        game->current.rotation = rotation;
        game->lock_frames = 0;
        emit(game, H2_DINOTETRIS_EVENT_ROTATE);
        return true;
    }
    return false;
}

bool move_horizontal(h2_dinotetris *game, int direction) {
    const int x = game->current.x + direction;
    if (collision(game, x, game->current.y, game->current.rotation)) {
        return false;
    }
    game->current.x = static_cast<int8_t>(x);
    game->lock_frames = 0;
    return true;
}

void reset(h2_dinotetris *game) {
    game->board = {};
    game->accumulator_us = 0;
    game->score = 0;
    game->lines_cleared = 0;
    game->level = 1;
    game->game_over_animation_ms = 0;
    game->restart_animation_ms = 0;
    game->drop_frames = 0;
    game->lock_frames = 0;
    game->action_frames = 0;
    game->horizontal_hold_frames = 0;
    game->horizontal_hold_direction = 0;
    game->action_pressed = false;
    game->fast_drop = false;
    game->game_over = false;
    game->restart_requested = false;
    game->next_shape = static_cast<uint8_t>(random_next(game) % 7u);
    spawn(game);
}
} // namespace

const h2_dinotetris_texts_t *h2_dinotetris_english_texts(void) {
    return &kEnglishTexts;
}

void DinoTetrisScene::init() {
    if (owner->initialized) return;
    pixelroot32::core::Scene::init();
    reset(owner);
    owner->initialized = true;
}

void DinoTetrisScene::update(unsigned long delta_ms) {
    if (owner->game_over) {
        if (owner->restart_requested) {
            owner->restart_animation_ms += static_cast<uint32_t>(delta_ms);
            if (owner->restart_animation_ms >= kPopupAnimationMs) reset(owner);
        } else {
            owner->game_over_animation_ms = std::min(
                owner->game_over_animation_ms + static_cast<uint32_t>(delta_ms),
                kPopupAnimationMs);
        }
        return;
    }
    owner->accumulator_us += static_cast<uint64_t>(delta_ms) * 1000u;
    uint8_t steps = 0;
    while (!owner->game_over && owner->accumulator_us >= kFrameUs &&
           steps++ < 8) {
        step();
        owner->accumulator_us -= kFrameUs;
    }
}

void DinoTetrisScene::step() {
    if (owner->action_pressed && !owner->fast_drop &&
        ++owner->action_frames >= kLongPressFrames) {
        owner->fast_drop = true;
        emit(owner, H2_DINOTETRIS_EVENT_FAST_DROP);
    }
    if (owner->horizontal_hold_direction != 0 &&
        ++owner->horizontal_hold_frames >= kHorizontalRepeatDelayFrames &&
        (owner->horizontal_hold_frames - kHorizontalRepeatDelayFrames) %
                kHorizontalRepeatFrames ==
            0) {
        (void)move_horizontal(owner, owner->horizontal_hold_direction);
    }
    if (collision(owner, owner->current.x, owner->current.y + 1,
                  owner->current.rotation)) {
        if (++owner->lock_frames >= kLockDelayFrames) lock_piece(owner);
        return;
    }
    owner->lock_frames = 0;
    const int interval = owner->fast_drop ? kDropFast : drop_interval(owner);
    if (++owner->drop_frames >= interval) {
        owner->drop_frames = 0;
        move_down(owner);
    }
}

void DinoTetrisScene::draw(pixelroot32::graphics::Renderer &renderer) {
    blit_background(renderer);
    renderer.drawFilledRectangleW(kBoardX, kBoardY, kCols * kCell, kRows * kCell, 0x18c5);
    for (int row = 0; row < kRows; ++row) for (int col = 0; col < kCols; ++col) {
        uint8_t value = owner->board[row][col];
        const int local_row = row - owner->current.y;
        const int local_col = col - owner->current.x;
        if (local_row >= 0 && local_row < 4 &&
            local_col >= 0 && local_col < 4 &&
            shape_cell(owner->current.type, owner->current.rotation, local_row, local_col))
            value = static_cast<uint8_t>(owner->current.type + 1);
        const uint16_t color = value == 0 ? 0x0842 : kColors[value - 1];
        renderer.drawFilledRectangleW(kBoardX + col * kCell, kBoardY + row * kCell,
                                      kCell - 1, kCell - 1, color);
    }
    char text[24];
    draw_info_text(owner, renderer, owner->config.texts->score, 14,
                   pixelroot32::graphics::Color::Gray, 16);
    std::snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(owner->score));
    h2_game_text_span_t number{text, std::strlen(text)};
    draw_info_text(owner, renderer, number, 36, pixelroot32::graphics::Color::White, 16);
    draw_info_text(owner, renderer, owner->config.texts->level, 66,
                   pixelroot32::graphics::Color::Gray, 16);
    std::snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(owner->level));
    number = {text, std::strlen(text)};
    draw_info_text(owner, renderer, number, 88, pixelroot32::graphics::Color::White, 16);
    draw_info_text(owner, renderer, owner->config.texts->next, 118,
                   pixelroot32::graphics::Color::Gray, 16);
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col)
        if (shape_cell(owner->next_shape, 0, row, col))
            renderer.drawFilledRectangleW(kPreviewX + col * 10, kPreviewY + row * 10, 9, 9,
                                          kColors[owner->next_shape]);
    if (owner->game_over) {
        const int offset = game_over_popup_offset(owner);
        darken_for_game_over(renderer, offset);
        draw_centered_text(owner, renderer, owner->config.texts->game_over, 82 + offset,
                           pixelroot32::graphics::Color::White, 24);
        std::snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(owner->score));
        number = {text, std::strlen(text)};
        draw_centered_three_parts(owner, renderer, owner->config.texts->score,
                                  kColonSpace, number, 120 + offset, 16);
        std::snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(owner->level));
        number = {text, std::strlen(text)};
        draw_centered_three_parts(owner, renderer, owner->config.texts->experience,
                                  kSpacePlus, number, 148 + offset, 16);
    }
}

int h2_dinotetris_create(const h2_dinotetris_config_t *config,
                         h2_dinotetris_t **out_game) {
    if (out_game == nullptr) return H2_DINOTETRIS_ERR_INVALID_ARGUMENT;
    *out_game = nullptr;
    if (config == nullptr || config->text == nullptr || config->text->vtable == nullptr ||
        config->text->vtable->measure == nullptr || config->text->vtable->draw == nullptr ||
        config->texts == nullptr || !valid_text(config->texts->score) ||
        !valid_text(config->texts->level) || !valid_text(config->texts->next) ||
        !valid_text(config->texts->game_over) ||
        !valid_text(config->texts->experience)) {
        return H2_DINOTETRIS_ERR_INVALID_ARGUMENT;
    }
    *out_game = new (std::nothrow) h2_dinotetris(*config);
    if (*out_game == nullptr) return H2_DINOTETRIS_ERR_NO_MEMORY;
    (*out_game)->scene.init();
    return H2_DINOTETRIS_OK;
}

h2_game_scene_t *h2_dinotetris_scene(h2_dinotetris_t *game) {
    return game == nullptr ? nullptr : reinterpret_cast<h2_game_scene_t *>(&game->scene);
}

void h2_dinotetris_handle_input(h2_dinotetris_t *game,
                                const h2_game_input_event_t *event) {
    if (game == nullptr || event == nullptr) return;
    if (game->game_over) {
        if (event->button == H2_DINOTETRIS_BUTTON_ACTION &&
            (event->type == H2_GAME_INPUT_BUTTON_DOWN ||
             event->type == H2_GAME_INPUT_BUTTON_CLICK) &&
            game->game_over_animation_ms >= kPopupAnimationMs &&
            !game->restart_requested) {
            game->restart_requested = true;
            game->restart_animation_ms = 0;
        }
        return;
    }
    if (event->button == H2_DINOTETRIS_BUTTON_LEFT &&
        (event->type == H2_GAME_INPUT_BUTTON_DOWN || event->type == H2_GAME_INPUT_BUTTON_CLICK)) {
        (void)move_horizontal(game, -1);
        if (event->type == H2_GAME_INPUT_BUTTON_DOWN) {
            game->horizontal_hold_direction = -1;
            game->horizontal_hold_frames = 0;
        }
    } else if (event->button == H2_DINOTETRIS_BUTTON_LEFT &&
               event->type == H2_GAME_INPUT_BUTTON_UP) {
        if (game->horizontal_hold_direction == -1) {
            game->horizontal_hold_direction = 0;
            game->horizontal_hold_frames = 0;
        }
    } else if (event->button == H2_DINOTETRIS_BUTTON_RIGHT &&
               (event->type == H2_GAME_INPUT_BUTTON_DOWN || event->type == H2_GAME_INPUT_BUTTON_CLICK)) {
        (void)move_horizontal(game, 1);
        if (event->type == H2_GAME_INPUT_BUTTON_DOWN) {
            game->horizontal_hold_direction = 1;
            game->horizontal_hold_frames = 0;
        }
    } else if (event->button == H2_DINOTETRIS_BUTTON_RIGHT &&
               event->type == H2_GAME_INPUT_BUTTON_UP) {
        if (game->horizontal_hold_direction == 1) {
            game->horizontal_hold_direction = 0;
            game->horizontal_hold_frames = 0;
        }
    } else if (event->button == H2_DINOTETRIS_BUTTON_ACTION) {
        if (event->type == H2_GAME_INPUT_BUTTON_DOWN) {
            game->action_pressed = true;
            game->action_frames = 0;
        } else if (event->type == H2_GAME_INPUT_BUTTON_UP) {
            if (game->action_pressed && !game->fast_drop) rotate(game);
            game->action_pressed = false;
            game->action_frames = 0;
            game->fast_drop = false;
        } else if (event->type == H2_GAME_INPUT_BUTTON_CLICK) {
            rotate(game);
        }
    }
}

void h2_dinotetris_reset(h2_dinotetris_t *game) { if (game != nullptr) reset(game); }

int h2_dinotetris_get_result(const h2_dinotetris_t *game,
                             h2_dinotetris_result_t *out_result) {
    if (game == nullptr || out_result == nullptr) return H2_DINOTETRIS_ERR_INVALID_ARGUMENT;
    *out_result = {game->score, game->lines_cleared, game->level,
                   static_cast<uint8_t>(game->game_over)};
    return H2_DINOTETRIS_OK;
}

void h2_dinotetris_destroy(h2_dinotetris_t *game) {
    if (game == nullptr) return;
    delete game;
}
