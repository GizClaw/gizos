#include "dinodive_internal.hpp"

#include "graphics/Color.h"
#include "graphics/Renderer.h"
#include "math/MathUtil.h"
#include "physics/CollisionTypes.h"
#include "physics/PhysicsScheduler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace {
namespace math = pixelroot32::math;

constexpr int kScreen = 240;
constexpr int kPlayerSize = 60;
constexpr int kPlayerFeet = 58;
constexpr int kPlatformWidth = 80;
constexpr int kPlatformHeight = 12;
constexpr int kPlatformGap = 65;
constexpr uint64_t kPhysicsStepUs = pixelroot32::physics::PhysicsScheduler::FIXED_DT_MICROS;
constexpr uint8_t kMaxCatchUpSteps = pixelroot32::physics::PhysicsScheduler::MAX_STEPS_BACKLOG;
constexpr pixelroot32::physics::CollisionLayer kPlayerLayer = 1u << 0u;
constexpr pixelroot32::physics::CollisionLayer kPlatformLayer = 1u << 1u;
constexpr pixelroot32::physics::CollisionLayer kHazardLayer = 1u << 2u;
constexpr math::Scalar kHorizontalSpeed = math::toScalar(84.0f);
constexpr math::Scalar kMaxFallSpeed = math::toScalar(150.0f);
constexpr math::Scalar kBaseScrollSpeed = math::toScalar(28.0f);
constexpr math::Scalar kScrollSpeedStep = math::toScalar(6.0f);
constexpr math::Scalar kMaxScrollSpeed = math::toScalar(64.0f);
constexpr math::Scalar kFixedDeltaSeconds = pixelroot32::physics::CollisionSystem::FIXED_DT;
constexpr int kPlatformImageWidth = 110;
constexpr int kPlatformImageHeight = 42;
constexpr int kPlatformImageMargin = 15;

const h2_dinodive_texts_t kEnglishTexts = {
    H2_GAME_TEXT_LITERAL("FLOOR"),
    H2_GAME_TEXT_LITERAL("PRESS START"),
};
const h2_game_text_span_t kSpace H2_GAME_TEXT_LITERAL(" ");

bool valid_text(h2_game_text_span_t text) {
    return text.data != nullptr && text.byte_len != 0u &&
           h2_game_text_validate_utf8(text) == H2_GAME_TEXT_OK;
}

h2_game_text_surface_t text_surface(pixelroot32::graphics::Renderer &renderer) {
    return {renderer.getDrawSurface().getPixelBuffer(), kScreen, kScreen, kScreen,
            static_cast<size_t>(kScreen * kScreen)};
}

int measure_text(const h2_dinodive *game, h2_game_text_span_t text, uint16_t line_height) {
    h2_game_text_metrics_t metrics{};
    return h2_game_text_measure(game->config.text, text, line_height, &metrics) ==
            H2_GAME_TEXT_OK
        ? metrics.width_px
        : -1;
}

int text_advance(const h2_dinodive *game, h2_game_text_span_t text, uint16_t line_height) {
    h2_game_text_metrics_t metrics{};
    return h2_game_text_measure(game->config.text, text, line_height, &metrics) ==
            H2_GAME_TEXT_OK
        ? metrics.advance_px
        : -1;
}

void draw_text(
    const h2_dinodive *game,
    pixelroot32::graphics::Renderer &renderer,
    h2_game_text_span_t text,
    int x,
    int y,
    uint16_t line_height) {
    const h2_game_text_surface_t surface = text_surface(renderer);
    const uint16_t color = pixelroot32::graphics::resolveColor(
        pixelroot32::graphics::Color::White,
        pixelroot32::graphics::PaletteContext::Sprite);
    (void)h2_game_text_draw(
        game->config.text, &surface, text, x, y, {color, line_height});
}

void draw_centered_text(
    const h2_dinodive *game,
    pixelroot32::graphics::Renderer &renderer,
    h2_game_text_span_t text,
    int y,
    uint16_t line_height) {
    const int width = measure_text(game, text, line_height);
    if (width >= 0) draw_text(game, renderer, text, (kScreen - width) / 2, y, line_height);
}

constexpr uint8_t kBackgroundArgb4444[] = {
#include "dinodive_background_argb4444.inc"
};

constexpr uint8_t kPlatformArgb4444[] = {
#include "dinodive_platform_argb4444.inc"
};

constexpr uint8_t kSpikeArgb4444[] = {
#include "dinodive_spike_argb4444.inc"
};

static_assert(sizeof(kBackgroundArgb4444) == kScreen * kScreen * 2u);
static_assert(sizeof(kPlatformArgb4444) == kPlatformImageWidth * kPlatformImageHeight * 2u);
static_assert(sizeof(kSpikeArgb4444) == kDinoDiveSpikeWidth * kDinoDiveSpikeHeight * 2u);

uint32_t next_random(h2_dinodive *game) {
    uint32_t value = game->random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    game->random_state = value == 0 ? 0x6d2b79f5u : value;
    return game->random_state;
}

int random_range(h2_dinodive *game, int minimum, int maximum) {
    return minimum + static_cast<int>(next_random(game) % static_cast<uint32_t>(maximum - minimum + 1));
}

math::Scalar scroll_speed_for(int32_t floor_count) {
    const math::Scalar speed = kBaseScrollSpeed + kScrollSpeedStep * math::toScalar(floor_count / 10);
    return math::min(speed, kMaxScrollSpeed);
}

void emit(h2_dinodive *game, h2_dinodive_event_t event) {
    if (game->config.audio != nullptr) {
        const auto *recipe = event == H2_DINODIVE_EVENT_FALL ? &h2_dinodive_fall_recipe : &h2_dinodive_game_over_recipe;
        const int rc = event == H2_DINODIVE_EVENT_GAME_OVER
            ? h2_game_audio_play_latest(game->config.audio, recipe)
            : h2_game_audio_play(game->config.audio, recipe);
        (void)rc;
    }
    if (game->config.event_callback != nullptr) game->config.event_callback(game->config.event_user, event);
}

void sync_hazard(h2_dinodive *game, int index) {
    auto &platform = game->platforms[index];
    auto &hazard = game->hazards[index];
    const math::Scalar visual_x = platform.position.x +
        (platform.spike_on_right ? math::toScalar(kDinoDiveSpikeWidth) : math::toScalar(0));
    hazard.setPosition({visual_x + math::toScalar(2), platform.position.y - math::toScalar(9)});
    hazard.setVisible(platform.has_spike);
}

void spawn_platform(h2_dinodive *game, int index, math::Scalar y, bool safe) {
    auto &platform = game->platforms[index];
    platform.setPosition({math::toScalar(random_range(game, 0, kScreen - kPlatformWidth)), y});
    platform.has_spike = !safe && random_range(game, 0, 100) < 30;
    platform.spike_on_right = platform.has_spike && random_range(game, 0, 100) >= 50;
    sync_hazard(game, index);
}

void reset_game(h2_dinodive *game) {
    game->random_state = game->initial_seed;
    game->floor_count = 0;
    game->direction = 1;
    game->started = false;
    game->game_over = false;
    game->was_on_floor = true;
    game->elapsed_us = 0;
    game->accumulator_us = 0;

    const math::Scalar scroll_speed = scroll_speed_for(0);
    for (int i = 0; i < kDinoDivePlatformCount; ++i) {
        spawn_platform(game, i, math::toScalar(120 + i * kPlatformGap), i == 0);
        game->platforms[i].setVelocity(math::toScalar(0), -scroll_speed);
        game->hazards[i].setVelocity(math::toScalar(0), -scroll_speed);
    }

    game->player.setVelocity(math::toScalar(0), math::toScalar(0));
    game->player.contacted_floor = false;
    game->player.contacted_hazard = false;
    game->player.floor_contact = nullptr;
    game->player.setPosition({
        game->platforms[0].position.x + math::toScalar((kPlatformWidth - kPlayerSize) / 2),
        game->platforms[0].position.y - math::toScalar(kPlayerFeet),
    });
}

void game_over(h2_dinodive *game) {
    if (game->game_over) return;
    game->game_over = true;
    game->player.setVelocity(math::toScalar(0), math::toScalar(0));
    emit(game, H2_DINODIVE_EVENT_GAME_OVER);
}

bool valid_clip(const pixa_asset_t *asset, const char *name, bool require_loop = false) {
    pixa_clip_t clip{};
    return pixa_find_clip(asset, name, &clip) == PIXA_OK && clip.frame_count > 0 &&
        (!require_loop || clip.loop != 0);
}

bool valid_player_frames(const pixa_asset_t *asset, uint8_t *scratch, size_t scratch_len) {
    for (const char *name : {"run_left", "run_right"}) {
        pixa_clip_t clip{};
        if (pixa_find_clip(asset, name, &clip) != PIXA_OK) return false;
        for (uint32_t frame = 0; frame < clip.frame_count; ++frame) {
            if (pixa_decode_clip_frame_bgra(asset, name, frame, scratch, scratch_len) != PIXA_OK) {
                return false;
            }
        }
    }
    return true;
}

void blit_argb4444(pixelroot32::graphics::Renderer &renderer, const uint8_t *pixels,
    uint16_t width, uint16_t height, int x, int y) {
    (void)pixa_blit_argb4444_to_rgb565(renderer.getDrawSurface().getPixelBuffer(), kScreen, kScreen, kScreen,
        pixels, width, height, static_cast<int16_t>(x), static_cast<int16_t>(y));
}

void draw_background(pixelroot32::graphics::Renderer &renderer) {
    blit_argb4444(renderer, kBackgroundArgb4444, kScreen, kScreen, 0, 0);
}

void draw_platform(pixelroot32::graphics::Renderer &renderer, int x, int y) {
    blit_argb4444(renderer, kPlatformArgb4444, kPlatformImageWidth, kPlatformImageHeight,
        x - kPlatformImageMargin, y - kPlatformImageMargin);
}

void blit_player(h2_dinodive *game, pixelroot32::graphics::Renderer &renderer,
    const char *name, uint32_t time_ms, int x, int y) {
    pixa_clip_t clip{};
    uint32_t frame = 0;
    const pixa_asset_t *asset = game->config.player;
    if (pixa_find_clip(asset, name, &clip) != PIXA_OK ||
        pixa_frame_index_at_ms(asset, &clip, time_ms, &frame) != PIXA_OK) return;
    if (!game->decoded_player_frame_valid || game->decoded_player_direction != game->direction ||
        game->decoded_player_frame != frame) {
        if (pixa_decode_clip_frame_bgra(asset, name, frame, game->player_bgra.get(),
                pixa_canvas_bgra_bytes(asset->canvas)) != PIXA_OK ||
            pixa_bgra_to_argb4444(game->player_bgra.get(), pixa_canvas_bgra_bytes(asset->canvas),
                game->player_argb4444.get(), pixa_canvas_argb4444_bytes(asset->canvas)) != PIXA_OK) return;
        game->decoded_player_direction = game->direction;
        game->decoded_player_frame = frame;
        game->decoded_player_frame_valid = true;
    }
    blit_argb4444(renderer, game->player_argb4444.get(), asset->canvas.width, asset->canvas.height, x, y);
}

}

const h2_dinodive_texts_t *h2_dinodive_english_texts(void) {
    return &kEnglishTexts;
}

DinoDivePlayerActor::DinoDivePlayerActor(h2_dinodive *owner)
    : RigidActor(math::toScalar(0), math::toScalar(0), kPlayerSize, kPlayerSize), owner_(owner) {
    setCollisionLayer(kPlayerLayer);
    setCollisionMask(kPlatformLayer | kHazardLayer);
    setHitboxOffset({math::toScalar(20), math::toScalar(5)});
    setHitboxDimensions(math::toScalar(26), math::toScalar(53));
    setGravityScale(math::toScalar(1.3f));
    setRestitution(math::toScalar(0));
    setFriction(math::toScalar(0));
    setBounce(false);
}

void DinoDivePlayerActor::update(unsigned long delta_ms) {
    (void)delta_ms;
    math::Vector2 next_velocity = getVelocity();
    next_velocity.x = kHorizontalSpeed * math::toScalar(owner_->direction);
    next_velocity.y = math::min(next_velocity.y, kMaxFallSpeed);
    setVelocity(next_velocity);
}

void DinoDivePlayerActor::draw(pixelroot32::graphics::Renderer &renderer) { (void)renderer; }

void DinoDivePlayerActor::onCollision(pixelroot32::core::Actor *other) {
    if (other == nullptr) return;
    if (other->isInLayer(kPlatformLayer)) {
        contacted_floor = true;
        floor_contact = static_cast<pixelroot32::core::PhysicsActor *>(other);
    }
    if (other->isInLayer(kHazardLayer)) contacted_hazard = true;
}

DinoDivePlatformActor::DinoDivePlatformActor()
    : PhysicsActor(math::toScalar(0), math::toScalar(0), kPlatformWidth, kPlatformHeight) {
    setBodyType(pixelroot32::core::PhysicsBodyType::KINEMATIC);
    setCollisionLayer(kPlatformLayer);
    setCollisionMask(kPlayerLayer);
    setOneWay(true);
    setBounce(false);
}

void DinoDivePlatformActor::update(unsigned long delta_ms) {
    (void)delta_ms;
}
void DinoDivePlatformActor::draw(pixelroot32::graphics::Renderer &renderer) { (void)renderer; }

DinoDiveHazardActor::DinoDiveHazardActor()
    : PhysicsActor(math::toScalar(0), math::toScalar(0), kDinoDiveSpikeWidth - 4, kDinoDiveSpikeHeight - 4) {
    setBodyType(pixelroot32::core::PhysicsBodyType::KINEMATIC);
    setCollisionLayer(kHazardLayer);
    setCollisionMask(kPlayerLayer);
    setSensor(true);
    setBounce(false);
}

void DinoDiveHazardActor::update(unsigned long delta_ms) {
    (void)delta_ms;
}
void DinoDiveHazardActor::draw(pixelroot32::graphics::Renderer &renderer) { (void)renderer; }

h2_dinodive::h2_dinodive(const h2_dinodive_config_t &value)
    : config(value), scene(this), player(this) {}

void DinoDiveScene::init() {
    pixelroot32::core::Scene::init();
    addEntity(&owner_->player);
    for (int i = 0; i < kDinoDivePlatformCount; ++i) {
        addEntity(&owner_->platforms[i]);
        addEntity(&owner_->hazards[i]);
    }
    reset_game(owner_);
}

void DinoDiveScene::update(unsigned long delta_ms) {
    if (!owner_->started || owner_->game_over) return;

    owner_->accumulator_us += static_cast<uint64_t>(delta_ms) * 1000u;
    uint8_t steps = 0;
    while (owner_->accumulator_us >= kPhysicsStepUs && steps < kMaxCatchUpSteps) {
        stepPhysics();
        owner_->accumulator_us -= kPhysicsStepUs;
        ++steps;
    }
}

void DinoDiveScene::stepPhysics() {
    if (owner_->game_over) return;

    const math::Scalar scroll_speed = scroll_speed_for(owner_->floor_count);
    for (int i = 0; i < kDinoDivePlatformCount; ++i) {
        owner_->platforms[i].setVelocity(math::toScalar(0), -scroll_speed);
        owner_->hazards[i].setVelocity(math::toScalar(0), -scroll_speed);
    }

    owner_->player.contacted_floor = false;
    owner_->player.contacted_hazard = false;
    owner_->player.floor_contact = nullptr;
    owner_->player.update(static_cast<unsigned long>(kPhysicsStepUs / 1000u));
    collisionSystem.update();
    owner_->player.setVelocity(owner_->player.getVelocityX(),
        math::min(owner_->player.getVelocityY(), kMaxFallSpeed));

    if (owner_->player.contacted_floor) {
        const auto floor_box = owner_->player.floor_contact->getHitBox();
        const math::Scalar hitbox_height = owner_->player.getHitboxHeight();
        owner_->player.setPosition({
            owner_->player.position.x,
            floor_box.position.y - owner_->player.getHitboxOffset().y - hitbox_height,
        });
        owner_->player.setVelocity(owner_->player.getVelocityX(), math::toScalar(0));
    }
    if (owner_->was_on_floor && !owner_->player.contacted_floor) emit(owner_, H2_DINODIVE_EVENT_FALL);
    owner_->was_on_floor = owner_->player.contacted_floor;

    if (owner_->player.position.x < math::toScalar(-kPlayerSize)) {
        owner_->player.setPosition({math::toScalar(kScreen), owner_->player.position.y});
    } else if (owner_->player.position.x > math::toScalar(kScreen)) {
        owner_->player.setPosition({math::toScalar(-kPlayerSize), owner_->player.position.y});
    }

    for (int i = 0; i < kDinoDivePlatformCount; ++i) {
        auto &platform = owner_->platforms[i];
        const math::Vector2 motion = platform.getVelocity() * kFixedDeltaSeconds;
        platform.setPosition(platform.position + motion);
        if (owner_->player.floor_contact == &platform) {
            owner_->player.setPosition(owner_->player.position + motion);
        }
        sync_hazard(owner_, i);
        if (platform.position.y >= math::toScalar(-kPlatformHeight)) continue;
        math::Scalar lowest = math::toScalar(0);
        for (const auto &candidate : owner_->platforms) lowest = math::max(lowest, candidate.position.y);
        spawn_platform(owner_, i, math::max(math::toScalar(kScreen), lowest + math::toScalar(kPlatformGap)), false);
        ++owner_->floor_count;
    }

    if (owner_->player.contacted_hazard) {
        game_over(owner_);
        return;
    }
    if (owner_->player.position.y < math::toScalar(-kPlayerSize) ||
        owner_->player.position.y > math::toScalar(kScreen)) {
        game_over(owner_);
        return;
    }
    owner_->elapsed_us += kPhysicsStepUs;
}

void DinoDiveScene::draw(pixelroot32::graphics::Renderer &renderer) {
    draw_background(renderer);
    for (const auto &platform : owner_->platforms) {
        const int x = math::roundToInt(platform.position.x);
        const int y = math::roundToInt(platform.position.y);
        draw_platform(renderer, x, y);
        if (platform.has_spike) {
            const int spike_x = x + (platform.spike_on_right ? kDinoDiveSpikeWidth : 0);
            blit_argb4444(renderer, kSpikeArgb4444, kDinoDiveSpikeWidth, kDinoDiveSpikeHeight,
                spike_x, y - kDinoDiveSpikeHeight);
        }
    }
    const char *player_clip = owner_->direction < 0 ? "run_left" : "run_right";
    blit_player(owner_, renderer, player_clip, static_cast<uint32_t>(owner_->elapsed_us / 1000u),
        math::roundToInt(owner_->player.position.x), math::roundToInt(owner_->player.position.y));
    char score[24];
    std::snprintf(score, sizeof(score), "%ld", static_cast<long>(owner_->floor_count));
    const h2_game_text_span_t score_span{score, std::strlen(score)};
    draw_text(owner_, renderer, owner_->config.texts->floor, 82, 12, 8);
    const int floor_advance = text_advance(owner_, owner_->config.texts->floor, 8);
    const int space_advance = text_advance(owner_, kSpace, 8);
    const int64_t score_x = static_cast<int64_t>(82) + floor_advance + space_advance;
    if (floor_advance >= 0 && space_advance >= 0 && score_x <= INT32_MAX) {
        draw_text(owner_, renderer, score_span, static_cast<int>(score_x), 12, 8);
    }
    if (!owner_->started) {
        draw_centered_text(owner_, renderer, owner_->config.texts->press_start, 92, 16);
    }
}

int h2_dinodive_create(const h2_dinodive_config_t *config, h2_dinodive_t **out_game) {
    if (out_game == nullptr) return H2_DINODIVE_ERR_INVALID_ARG;
    *out_game = nullptr;
    if (config == nullptr || config->player == nullptr || config->text == nullptr ||
        config->text->vtable == nullptr || config->text->vtable->measure == nullptr ||
        config->text->vtable->draw == nullptr || config->texts == nullptr ||
        !valid_text(config->texts->floor) || !valid_text(config->texts->press_start)) {
        return H2_DINODIVE_ERR_INVALID_ARG;
    }
    if (config->player->canvas.width != kPlayerSize || config->player->canvas.height != kPlayerSize ||
        !valid_clip(config->player, "run_left", true) || !valid_clip(config->player, "run_right", true)) {
        return H2_DINODIVE_ERR_ASSET;
    }

    auto *game = new (std::nothrow) h2_dinodive_t(*config);
    if (game == nullptr) return H2_DINODIVE_ERR_NO_MEMORY;
    const size_t player_bgra_size = pixa_canvas_bgra_bytes(config->player->canvas);
    const size_t player_argb_size = pixa_canvas_argb4444_bytes(config->player->canvas);
    game->player_bgra.reset(new (std::nothrow) uint8_t[player_bgra_size]);
    game->player_argb4444.reset(new (std::nothrow) uint8_t[player_argb_size]);
    if (!game->player_bgra || !game->player_argb4444) {
        delete game;
        return H2_DINODIVE_ERR_NO_MEMORY;
    }
    if (!valid_player_frames(config->player, game->player_bgra.get(), player_bgra_size)) {
        delete game;
        return H2_DINODIVE_ERR_ASSET;
    }
    game->initial_seed = config->seed == 0 ? 1 : config->seed;
    game->scene.init();
    *out_game = game;
    return H2_DINODIVE_OK;
}

h2_game_scene_t *h2_dinodive_scene(h2_dinodive_t *game) {
    return game == nullptr ? nullptr : reinterpret_cast<h2_game_scene_t *>(&game->scene);
}

void h2_dinodive_handle_input(h2_dinodive_t *game, const h2_game_input_event_t *event) {
    if (game == nullptr || event == nullptr || game->game_over) return;
    if (event->type != H2_GAME_INPUT_BUTTON_DOWN) return;
    if (event->button == H2_DINODIVE_BUTTON_ACTION) {
        game->started = true;
        return;
    }
    if (!game->started) return;
    if (event->button == H2_DINODIVE_BUTTON_LEFT) game->direction = -1;
    else if (event->button == H2_DINODIVE_BUTTON_RIGHT) game->direction = 1;
}

void h2_dinodive_reset(h2_dinodive_t *game) {
    if (game != nullptr) reset_game(game);
}

int h2_dinodive_get_result(const h2_dinodive_t *game, h2_dinodive_result_t *out) {
    if (game == nullptr || out == nullptr) return H2_DINODIVE_ERR_INVALID_ARG;
    *out = {game->floor_count, game->game_over ? 1 : 0};
    return H2_DINODIVE_OK;
}

void h2_dinodive_destroy(h2_dinodive_t *game) {
    if (game != nullptr) delete game;
}
