#include "polygon_battle_internal.hpp"

#include "graphics/Renderer.h"

#include <algorithm>
#include <cstdio>
#include <new>

namespace {

constexpr int kScreen = 240;
constexpr uint64_t kStepUs = 16667u;
constexpr int32_t kPlayerY = 210000;
constexpr uint16_t kWhite = 0xffff;
constexpr uint16_t kBackground = 0x0042;
constexpr uint16_t kPanel = 0x10a5;
constexpr uint16_t kPanelRaised = 0x1949;
constexpr uint16_t kEnemyFill = 0x8b5f;
constexpr uint16_t kEnemyOutline = 0xde5f;
constexpr uint16_t kCyan = 0x57bf;
constexpr uint16_t kRed = 0xfa6d;
constexpr uint16_t kOrange = 0xfce8;
constexpr uint16_t kYellow = 0xfe29;
constexpr uint16_t kMuted = 0xa598;
constexpr uint16_t kPurple = 0x59f7;
constexpr uint16_t kShipFill = 0x475c;
constexpr uint16_t kShipOutline = 0xcfff;
constexpr uint16_t kDark = 0x0842;
constexpr uint8_t kDifficultyProfileCount = 5u;

constexpr h2_polygon_battle_texts_t kEnglishTexts = {
    H2_GAME_TEXT_LITERAL("POLYGON BATTLE"),
    H2_GAME_TEXT_LITERAL("ACTION: START"),
    H2_GAME_TEXT_LITERAL("LIFE"),
    H2_GAME_TEXT_LITERAL("SCORE"),
    H2_GAME_TEXT_LITERAL("WAVE"),
    H2_GAME_TEXT_LITERAL("SPREAD"),
    H2_GAME_TEXT_LITERAL("PIERCE"),
    H2_GAME_TEXT_LITERAL("RICOCHET"),
    H2_GAME_TEXT_LITERAL("POWER"),
    H2_GAME_TEXT_LITERAL("GAME OVER"),
    H2_GAME_TEXT_LITERAL("DESTROYED"),
    H2_GAME_TEXT_LITERAL("MAX COMBO"),
    H2_GAME_TEXT_LITERAL("ACTION: RETRY"),
};

bool validText(h2_game_text_span_t text) {
    return text.data != nullptr && text.byte_len != 0u &&
        h2_game_text_validate_utf8(text) == H2_GAME_TEXT_OK;
}

int32_t stepDistance(int32_t velocity_px_per_second) {
    return static_cast<int32_t>(
        static_cast<int64_t>(velocity_px_per_second) *
        static_cast<int64_t>(kStepUs) / 1000);
}

uint32_t elapsedMs(const h2_polygon_battle *game) {
    return static_cast<uint32_t>(
        std::min<uint64_t>(game->elapsed_us / 1000u, UINT32_MAX));
}

struct Point { int16_t x; int16_t y; };
constexpr Point kTriangle[] = {{0, -1000}, {866, 500}, {-866, 500}};
constexpr Point kSquare[] = {{-850, -850}, {850, -850}, {850, 850}, {-850, 850}};
constexpr Point kPentagon[] = {{0, -1000}, {951, -309}, {588, 809}, {-588, 809}, {-951, -309}};
constexpr Point kHexagon[] = {{0, -1000}, {866, -500}, {866, 500}, {0, 1000}, {-866, 500}, {-866, -500}};

uint32_t nextRandom(h2_polygon_battle *game) {
    uint32_t value = game->random_state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    game->random_state = value == 0u ? 0x6d2b79f5u : value;
    return game->random_state;
}

void notify(h2_polygon_battle *game, const h2_polygon_battle_event_t &event) {
    if (game->config.event_callback != nullptr) {
        game->config.event_callback(game->config.event_user, &event);
    }
}

void play(h2_polygon_battle *game,
          const h2_game_audio_recipe_t &recipe,
          bool priority = false) {
    if (game->config.audio == nullptr || game->audio_failed) return;
    const int rc = priority
        ? h2_game_audio_play_latest(game->config.audio, &recipe)
        : h2_game_audio_play(game->config.audio, &recipe);
    if (rc != H2_GAME_AUDIO_OK && rc != H2_GAME_AUDIO_ERR_OVERFLOW) {
        game->audio_failed = true;
        notify(game, {H2_POLYGON_BATTLE_EVENT_AUDIO_ERROR, 0u,
                      H2_POLYGON_BATTLE_SHAPE_CIRCLE, 0u, 0u, game->wave});
    }
}

uint8_t shapeHp(h2_polygon_battle_shape_t shape) {
    return shape == H2_POLYGON_BATTLE_SHAPE_CIRCLE ? 1u : static_cast<uint8_t>(shape);
}

uint16_t shapeScore(h2_polygon_battle_shape_t shape) {
    switch (shape) {
    case H2_POLYGON_BATTLE_SHAPE_CIRCLE: return 100u;
    case H2_POLYGON_BATTLE_SHAPE_TRIANGLE: return 240u;
    case H2_POLYGON_BATTLE_SHAPE_SQUARE: return 360u;
    case H2_POLYGON_BATTLE_SHAPE_PENTAGON: return 500u;
    case H2_POLYGON_BATTLE_SHAPE_HEXAGON: return 700u;
    }
    return 0u;
}

void clearActors(h2_polygon_battle *game) {
    game->enemies = {};
    game->projectiles = {};
    game->enemy_projectiles = {};
    game->pickups = {};
}

uint8_t difficultyIndex(uint32_t wave) {
    if (wave <= 1u) return 0u;
    return static_cast<uint8_t>(std::min<uint32_t>(wave - 1u, kDifficultyProfileCount - 1u));
}

void spawnWave(h2_polygon_battle *game, uint32_t wave) {
    static constexpr uint8_t enemy_counts[5] = {6u, 7u, 8u, 9u, 10u};
    static constexpr uint16_t first_fire_frames[5] = {210u, 180u, 145u, 115u, 85u};
    static constexpr h2_polygon_battle_shape_t formations[5][10] = {
        {H2_POLYGON_BATTLE_SHAPE_CIRCLE, H2_POLYGON_BATTLE_SHAPE_CIRCLE,
         H2_POLYGON_BATTLE_SHAPE_CIRCLE, H2_POLYGON_BATTLE_SHAPE_TRIANGLE,
         H2_POLYGON_BATTLE_SHAPE_CIRCLE, H2_POLYGON_BATTLE_SHAPE_TRIANGLE},
        {H2_POLYGON_BATTLE_SHAPE_CIRCLE, H2_POLYGON_BATTLE_SHAPE_CIRCLE,
         H2_POLYGON_BATTLE_SHAPE_TRIANGLE, H2_POLYGON_BATTLE_SHAPE_TRIANGLE,
         H2_POLYGON_BATTLE_SHAPE_TRIANGLE, H2_POLYGON_BATTLE_SHAPE_SQUARE,
         H2_POLYGON_BATTLE_SHAPE_SQUARE},
        {H2_POLYGON_BATTLE_SHAPE_CIRCLE, H2_POLYGON_BATTLE_SHAPE_CIRCLE,
         H2_POLYGON_BATTLE_SHAPE_CIRCLE, H2_POLYGON_BATTLE_SHAPE_SQUARE,
         H2_POLYGON_BATTLE_SHAPE_SQUARE, H2_POLYGON_BATTLE_SHAPE_SQUARE,
         H2_POLYGON_BATTLE_SHAPE_PENTAGON, H2_POLYGON_BATTLE_SHAPE_PENTAGON},
        {H2_POLYGON_BATTLE_SHAPE_CIRCLE, H2_POLYGON_BATTLE_SHAPE_CIRCLE,
         H2_POLYGON_BATTLE_SHAPE_TRIANGLE, H2_POLYGON_BATTLE_SHAPE_TRIANGLE,
         H2_POLYGON_BATTLE_SHAPE_PENTAGON, H2_POLYGON_BATTLE_SHAPE_PENTAGON,
         H2_POLYGON_BATTLE_SHAPE_PENTAGON, H2_POLYGON_BATTLE_SHAPE_HEXAGON,
         H2_POLYGON_BATTLE_SHAPE_HEXAGON},
        {H2_POLYGON_BATTLE_SHAPE_CIRCLE, H2_POLYGON_BATTLE_SHAPE_TRIANGLE,
         H2_POLYGON_BATTLE_SHAPE_SQUARE, H2_POLYGON_BATTLE_SHAPE_PENTAGON,
         H2_POLYGON_BATTLE_SHAPE_HEXAGON, H2_POLYGON_BATTLE_SHAPE_TRIANGLE,
         H2_POLYGON_BATTLE_SHAPE_SQUARE, H2_POLYGON_BATTLE_SHAPE_PENTAGON,
         H2_POLYGON_BATTLE_SHAPE_HEXAGON, H2_POLYGON_BATTLE_SHAPE_CIRCLE},
    };
    game->enemies = {};
    game->enemy_projectiles = {};
    const uint8_t difficulty = difficultyIndex(wave);
    const uint8_t enemy_count = enemy_counts[difficulty];
    const uint8_t columns = static_cast<uint8_t>((enemy_count + 1u) / 2u);
    const int32_t spacing = 200000 / static_cast<int32_t>(columns - 1u);
    for (size_t index = 0; index < enemy_count; ++index) {
        auto &enemy = game->enemies[index];
        enemy.active = true;
        enemy.id = game->next_enemy_id++;
        enemy.shape = formations[difficulty][index];
        enemy.max_hp = shapeHp(enemy.shape);
        enemy.hp = enemy.max_hp;
        const uint8_t row = static_cast<uint8_t>(index / columns);
        const uint8_t column = static_cast<uint8_t>(index % columns);
        const uint8_t row_count = static_cast<uint8_t>(std::min<size_t>(
            columns, enemy_count - static_cast<size_t>(row) * columns));
        const int32_t row_offset = row_count < columns ? spacing / 2 : 0;
        enemy.x = 20000 + row_offset + static_cast<int32_t>(column) * spacing;
        enemy.y = 40000 + static_cast<int32_t>(row) * 42000;
        enemy.vx = static_cast<int16_t>((index & 1u) == 0u ? 24 : -24);
        enemy.fire_frames = static_cast<uint16_t>(first_fire_frames[difficulty] + index * 13u);
    }
    notify(game, {H2_POLYGON_BATTLE_EVENT_WAVE_STARTED, 0u,
                  H2_POLYGON_BATTLE_SHAPE_CIRCLE, 0u, 0u, wave});
}

void beginGame(h2_polygon_battle *game) {
    clearActors(game);
    game->phase = PolygonBattlePhase::Playing;
    game->score = 0;
    game->destroyed_count = 0;
    game->elapsed_us = 0;
    game->combo = 0;
    game->max_combo = 0;
    game->wave = 1;
    game->life = 3;
    game->spread = 1;
    game->pierce = 1;
    game->ricochet = 0;
    game->power = 1;
    game->shield = false;
    game->player_x = 120000;
    game->shot_frames = 0;
    game->invulnerable_frames = 0;
    game->wave_pause_frames = 0;
    game->next_enemy_id = 1;
    spawnWave(game, 1u);
}

void emitShot(h2_polygon_battle *game) {
    play(game, h2_polygon_battle_shot_recipe);
    notify(game, {H2_POLYGON_BATTLE_EVENT_SHOT, 0u,
                  H2_POLYGON_BATTLE_SHAPE_CIRCLE, 0u, 0u, game->wave});
}

void firePlayer(h2_polygon_battle *game) {
    const uint8_t count = game->spread >= 3u ? 3u : 1u;
    size_t free_count = 0;
    for (const auto &projectile : game->projectiles) if (!projectile.active) ++free_count;
    if (free_count < count) return;
    static constexpr int16_t vx[3] = {-60, 0, 60};
    static constexpr int16_t vy[3] = {-222, -230, -222};
    uint8_t made = 0;
    for (auto &projectile : game->projectiles) {
        if (projectile.active) continue;
        projectile = {};
        projectile.active = true;
        projectile.x = game->player_x;
        projectile.y = kPlayerY - 11000;
        const uint8_t angle = count == 1u ? 1u : made;
        projectile.vx = vx[angle];
        projectile.vy = vy[angle];
        projectile.hit_capacity = game->pierce >= 2u ? 2u : 1u;
        projectile.bounces = game->ricochet >= 2u ? 2u : 0u;
        projectile.damage = game->power >= 2u ? 2u : 1u;
        if (++made == count) break;
    }
    game->shot_frames = 13u;
    emitShot(game);
}

bool alreadyHit(const PolygonBattleProjectile &projectile, uint16_t id) {
    for (uint8_t index = 0; index < projectile.hit_count; ++index) {
        if (projectile.hit_ids[index] == id) return true;
    }
    return false;
}

void damageEnemy(h2_polygon_battle *game,
                 PolygonBattleEnemy &enemy,
                 PolygonBattleProjectile &projectile) {
    const uint8_t applied = std::min(enemy.hp, projectile.damage);
    enemy.hp = static_cast<uint8_t>(enemy.hp - applied);
    projectile.hit_ids[projectile.hit_count++] = enemy.id;
    notify(game, {H2_POLYGON_BATTLE_EVENT_ENEMY_HIT, enemy.id, enemy.shape,
                  applied, enemy.hp, game->wave});
    if (enemy.hp == 0u) {
        enemy.destroy_pending = true;
        enemy.destroy_frames = 5u;
    } else {
        play(game, h2_polygon_battle_hit_recipe);
    }
    if (projectile.hit_count >= projectile.hit_capacity) projectile.active = false;
}

void spawnEnemyShot(h2_polygon_battle *game,
                    const PolygonBattleEnemy &enemy,
                    int x_offset,
                    int16_t angle_offset) {
    for (auto &shot : game->enemy_projectiles) {
        if (shot.active) continue;
        const int32_t dx = game->player_x - enemy.x;
        const int32_t dy = kPlayerY - enemy.y;
        const int32_t scale = std::max<int32_t>(1, std::max(std::abs(dx), std::abs(dy)));
        static constexpr int16_t speeds[5] = {90, 105, 120, 135, 150};
        const int16_t speed = speeds[difficultyIndex(game->wave)];
        shot.active = true;
        shot.x = enemy.x + x_offset * 1000;
        shot.y = enemy.y + 10000;
        shot.vx = static_cast<int16_t>(dx * speed / scale + angle_offset);
        shot.vy = static_cast<int16_t>(std::max<int32_t>(70, dy * speed / scale));
        shot.source_shape = enemy.shape;
        return;
    }
}

void fireEnemy(h2_polygon_battle *game, PolygonBattleEnemy &enemy) {
    if (enemy.shape == H2_POLYGON_BATTLE_SHAPE_CIRCLE) return;
    if (enemy.shape == H2_POLYGON_BATTLE_SHAPE_TRIANGLE) {
        spawnEnemyShot(game, enemy, 0, 0);
    } else if (enemy.shape == H2_POLYGON_BATTLE_SHAPE_SQUARE) {
        spawnEnemyShot(game, enemy, -4, 0);
        spawnEnemyShot(game, enemy, 4, 0);
    } else if (enemy.shape == H2_POLYGON_BATTLE_SHAPE_PENTAGON) {
        spawnEnemyShot(game, enemy, 0, -32);
        spawnEnemyShot(game, enemy, 0, 0);
        spawnEnemyShot(game, enemy, 0, 32);
    } else {
        if (enemy.alternate_fire) {
            spawnEnemyShot(game, enemy, 0, -36);
            spawnEnemyShot(game, enemy, 0, 0);
            spawnEnemyShot(game, enemy, 0, 36);
        } else {
            spawnEnemyShot(game, enemy, 0, 0);
        }
        enemy.alternate_fire = !enemy.alternate_fire;
    }
    static constexpr uint16_t cooldown_frames[5] = {240u, 190u, 150u, 115u, 85u};
    enemy.fire_frames = static_cast<uint16_t>(
        cooldown_frames[difficultyIndex(game->wave)] + (nextRandom(game) % 40u));
}

void spawnPickup(h2_polygon_battle *game, const PolygonBattleEnemy &enemy) {
    if (nextRandom(game) % 100u >= 18u) return;
    for (auto &pickup : game->pickups) {
        if (pickup.active) continue;
        const uint32_t roll = nextRandom(game) % 9u;
        pickup.active = true;
        pickup.x = enemy.x;
        pickup.y = enemy.y;
        pickup.kind = roll == 8u ? PolygonBattlePickup::Shield
            : static_cast<PolygonBattlePickup>(roll / 2u);
        return;
    }
}

void finishEnemy(h2_polygon_battle *game, PolygonBattleEnemy &enemy) {
    spawnPickup(game, enemy);
    ++game->destroyed_count;
    ++game->combo;
    game->max_combo = std::max(game->max_combo, game->combo);
    const uint32_t multiplier_tenths = 10u + std::min<uint32_t>(10u, game->combo / 5u);
    game->score += shapeScore(enemy.shape) * multiplier_tenths / 10u;
    notify(game, {H2_POLYGON_BATTLE_EVENT_ENEMY_DESTROYED, enemy.id, enemy.shape,
                  0u, 0u, game->wave});
    play(game, h2_polygon_battle_destroy_recipe);
    enemy.active = false;
    enemy.destroy_pending = false;
}

void damagePlayer(h2_polygon_battle *game) {
    if (game->invulnerable_frames != 0u) return;
    game->combo = 0;
    if (game->shield) {
        game->shield = false;
        game->invulnerable_frames = 72u;
        play(game, h2_polygon_battle_shield_recipe, true);
    } else {
        if (game->life > 0u) --game->life;
        game->invulnerable_frames = 72u;
        play(game, h2_polygon_battle_player_hit_recipe, true);
    }
    notify(game, {H2_POLYGON_BATTLE_EVENT_PLAYER_HIT, 0u,
                  H2_POLYGON_BATTLE_SHAPE_CIRCLE, 0u, 0u, game->wave});
    if (game->life == 0u) {
        game->phase = PolygonBattlePhase::GameOver;
        game->action_down = false;
        clearActors(game);
        notify(game, {H2_POLYGON_BATTLE_EVENT_GAME_OVER, 0u,
                      H2_POLYGON_BATTLE_SHAPE_CIRCLE, 0u, 0u, game->wave});
    }
}

void collectPickup(h2_polygon_battle *game, PolygonBattlePickupState &pickup) {
    bool duplicate = false;
    switch (pickup.kind) {
    case PolygonBattlePickup::Spread: duplicate = game->spread >= 3u; game->spread = 3u; break;
    case PolygonBattlePickup::Pierce: duplicate = game->pierce >= 2u; game->pierce = 2u; break;
    case PolygonBattlePickup::Ricochet: duplicate = game->ricochet >= 2u; game->ricochet = 2u; break;
    case PolygonBattlePickup::Power: duplicate = game->power >= 2u; game->power = 2u; break;
    case PolygonBattlePickup::Shield: duplicate = game->shield; game->shield = true; break;
    }
    if (duplicate) game->score += pickup.kind == PolygonBattlePickup::Shield ? 300u : 500u;
    pickup.active = false;
    play(game, h2_polygon_battle_pickup_recipe);
    notify(game, {H2_POLYGON_BATTLE_EVENT_PICKUP_COLLECTED, 0u,
                  H2_POLYGON_BATTLE_SHAPE_CIRCLE, 0u, 0u, game->wave});
}

bool anyEnemy(const h2_polygon_battle *game) {
    for (const auto &enemy : game->enemies) if (enemy.active) return true;
    return false;
}

void drawText(const h2_polygon_battle *game,
              pixelroot32::graphics::Renderer &renderer,
              h2_game_text_span_t text,
              int x,
              int y,
              uint16_t color,
              uint16_t height = 8u) {
    const h2_game_text_surface_t surface{
        renderer.getDrawSurface().getPixelBuffer(), kScreen, kScreen, kScreen,
        static_cast<size_t>(kScreen * kScreen)};
    (void)h2_game_text_draw(game->config.text, &surface, text, x, y, {color, height});
}

void drawCentered(const h2_polygon_battle *game,
                  pixelroot32::graphics::Renderer &renderer,
                  h2_game_text_span_t text,
                  int y,
                  uint16_t color,
                  uint16_t height = 8u) {
    h2_game_text_metrics_t metrics{};
    if (h2_game_text_measure(game->config.text, text, height, &metrics) == H2_GAME_TEXT_OK) {
        drawText(game, renderer, text, (kScreen - metrics.width_px) / 2, y, color, height);
    }
}

const Point *vertices(h2_polygon_battle_shape_t shape, uint8_t &count) {
    count = shapeHp(shape);
    switch (shape) {
    case H2_POLYGON_BATTLE_SHAPE_TRIANGLE: return kTriangle;
    case H2_POLYGON_BATTLE_SHAPE_SQUARE: return kSquare;
    case H2_POLYGON_BATTLE_SHAPE_PENTAGON: return kPentagon;
    case H2_POLYGON_BATTLE_SHAPE_HEXAGON: return kHexagon;
    default: count = 0u; return nullptr;
    }
}

struct ScreenPoint { int x; int y; };

void fillPolygon(pixelroot32::graphics::DrawSurface &surface,
                 const ScreenPoint *points,
                 uint8_t count,
                 uint16_t color) {
    if (points == nullptr || count < 3u || count > 6u) return;
    int minimum_y = points[0].y;
    int maximum_y = points[0].y;
    for (uint8_t index = 1; index < count; ++index) {
        minimum_y = std::min(minimum_y, points[index].y);
        maximum_y = std::max(maximum_y, points[index].y);
    }
    for (int y = minimum_y; y <= maximum_y; ++y) {
        std::array<int, 6> intersections{};
        uint8_t intersection_count = 0;
        for (uint8_t index = 0; index < count; ++index) {
            const ScreenPoint a = points[index];
            const ScreenPoint b = points[(index + 1u) % count];
            const bool crosses = (a.y <= y && b.y > y) || (b.y <= y && a.y > y);
            if (!crosses) continue;
            intersections[intersection_count++] = a.x +
                (y - a.y) * (b.x - a.x) / (b.y - a.y);
        }
        for (uint8_t index = 1u; index < intersection_count; ++index) {
            const int value = intersections[index];
            uint8_t insertion = index;
            while (insertion > 0u && intersections[insertion - 1u] > value) {
                intersections[insertion] = intersections[insertion - 1u];
                --insertion;
            }
            intersections[insertion] = value;
        }
        for (uint8_t index = 0; index + 1u < intersection_count; index += 2u) {
            const int left = intersections[index];
            const int right = intersections[index + 1u];
            surface.drawFilledRectangle(left, y, right - left + 1, 1, color);
        }
    }
}

void outlinePolygon(pixelroot32::graphics::DrawSurface &surface,
                    const ScreenPoint *points,
                    uint8_t count,
                    uint16_t color) {
    for (uint8_t index = 0; index < count; ++index) {
        const ScreenPoint a = points[index];
        const ScreenPoint b = points[(index + 1u) % count];
        surface.drawLine(a.x, a.y, b.x, b.y, color);
    }
}

void buildPolygon(const Point *source,
                  uint8_t count,
                  int center_x,
                  int center_y,
                  int radius,
                  std::array<ScreenPoint, 6> &out_points) {
    for (uint8_t index = 0; index < count; ++index) {
        out_points[index] = {
            center_x + source[index].x * radius / 1000,
            center_y + source[index].y * radius / 1000,
        };
    }
}

void drawDiamond(pixelroot32::graphics::DrawSurface &surface,
                 int center_x,
                 int center_y,
                 int half_width,
                 int half_height,
                 uint16_t color) {
    const ScreenPoint points[] = {
        {center_x, center_y - half_height}, {center_x + half_width, center_y},
        {center_x, center_y + half_height}, {center_x - half_width, center_y},
    };
    fillPolygon(surface, points, 4u, color);
}

void drawEnemy(pixelroot32::graphics::Renderer &renderer, const PolygonBattleEnemy &enemy) {
    auto &surface = renderer.getDrawSurface();
    const int cx = enemy.x / 1000;
    const int cy = enemy.y / 1000;
    const uint8_t damaged = static_cast<uint8_t>(enemy.max_hp - enemy.hp);
    if (enemy.shape == H2_POLYGON_BATTLE_SHAPE_CIRCLE) {
        surface.drawFilledCircle(cx, cy, 11, kEnemyFill);
        surface.drawCircle(cx, cy, 11, damaged != 0u ? kRed : kEnemyOutline);
        if (damaged != 0u) surface.drawLine(cx - 7, cy - 7, cx + 5, cy + 6, kRed);
        return;
    }
    uint8_t count = 0;
    const Point *points = vertices(enemy.shape, count);
    std::array<ScreenPoint, 6> screen_points{};
    buildPolygon(points, count, cx, cy, 13, screen_points);
    fillPolygon(surface, screen_points.data(), count, kEnemyFill);
    outlinePolygon(surface, screen_points.data(), count, kEnemyOutline);
    for (uint8_t index = 0; index < count; ++index) {
        if (index >= damaged) {
            surface.drawFilledCircle(screen_points[index].x, screen_points[index].y, 2, kWhite);
        }
    }
    for (uint8_t index = 0; index < damaged; ++index) {
        const Point p = points[index];
        const Point previous = points[(index + count - 1u) % count];
        const Point next = points[(index + 1u) % count];
        const int px = screen_points[index].x;
        const int py = screen_points[index].y;
        surface.drawLine(px, py, cx + (p.x * 7 + previous.x * 3) * 13 / 10000,
                         cy + (p.y * 7 + previous.y * 3) * 13 / 10000, kRed);
        surface.drawLine(px, py, cx + (p.x * 7 + next.x * 3) * 13 / 10000,
                         cy + (p.y * 7 + next.y * 3) * 13 / 10000, kRed);
        surface.drawLine(px, py, cx + p.x * 4 / 1000, cy + p.y * 4 / 1000, kRed);
        surface.drawFilledCircle(px, py, 2, kRed);
    }
}

void drawShipAt(pixelroot32::graphics::Renderer &renderer,
                int x,
                int y,
                bool shield,
                bool dimmed) {
    auto &surface = renderer.getDrawSurface();
    const ScreenPoint body[] = {
        {x, y - 20}, {x + 16, y + 14}, {x + 6, y + 10},
        {x, y + 18}, {x - 6, y + 10}, {x - 16, y + 14},
    };
    fillPolygon(surface, body, 6u, dimmed ? kDark : kShipFill);
    outlinePolygon(surface, body, 6u, dimmed ? kDark : kShipOutline);
    surface.drawFilledCircle(x, y - 1, 4, kPurple);
    const ScreenPoint flame[] = {{x - 6, y + 15}, {x, y + 23}, {x + 6, y + 15}};
    fillPolygon(surface, flame, 3u, kOrange);
    if (shield) {
        surface.drawCircle(x, y, 20, kCyan);
        surface.drawCircle(x, y, 19, kCyan);
    }
}

void drawShip(pixelroot32::graphics::Renderer &renderer, const h2_polygon_battle *game) {
    const bool dimmed = game->invulnerable_frames != 0u &&
        (game->invulnerable_frames / 4u) % 2u == 0u;
    drawShipAt(renderer, game->player_x / 1000, kPlayerY / 1000, game->shield, dimmed);
}

void drawEnemyProjectile(pixelroot32::graphics::DrawSurface &surface,
                         const PolygonBattleEnemyProjectile &shot) {
    const int x = shot.x / 1000;
    const int y = shot.y / 1000;
    if (shot.source_shape == H2_POLYGON_BATTLE_SHAPE_TRIANGLE) {
        surface.drawFilledCircle(x, y, 3, kOrange);
        surface.drawPixel(x - 1, y - 1, kYellow);
    } else if (shot.source_shape == H2_POLYGON_BATTLE_SHAPE_SQUARE) {
        surface.drawFilledRectangle(x - 2, y - 3, 5, 7, kOrange);
        surface.drawPixel(x, y - 2, kYellow);
    } else if (shot.source_shape == H2_POLYGON_BATTLE_SHAPE_PENTAGON) {
        drawDiamond(surface, x, y, 4, 7, kOrange);
    } else {
        drawDiamond(surface, x, y, 5, 10, kOrange);
        surface.drawLine(x, y - 6, x, y + 5, kYellow);
    }
}

void drawPickup(const h2_polygon_battle *game,
                pixelroot32::graphics::Renderer &renderer,
                const PolygonBattlePickupState &pickup) {
    auto &surface = renderer.getDrawSurface();
    const int x = pickup.x / 1000;
    const int y = pickup.y / 1000;
    surface.drawFilledCircle(x, y, 12, kPanelRaised);
    surface.drawCircle(x, y, 12, kCyan);
    surface.drawCircle(x, y, 11, kCyan);
    if (pickup.kind == PolygonBattlePickup::Shield) {
        const ScreenPoint shield[] = {
            {x, y - 6}, {x + 6, y - 3}, {x + 5, y + 4},
            {x, y + 8}, {x - 5, y + 4}, {x - 6, y - 3},
        };
        outlinePolygon(surface, shield, 6u, kWhite);
    } else {
        static constexpr char labels[] = {'S', 'P', 'R', 'D'};
        const h2_game_text_span_t label{&labels[static_cast<uint8_t>(pickup.kind)], 1u};
        drawText(game, renderer, label, x - 2, y - 4, kWhite, 8u);
    }
}

void drawRightAligned(const h2_polygon_battle *game,
                      pixelroot32::graphics::Renderer &renderer,
                      h2_game_text_span_t text,
                      int right,
                      int y,
                      uint16_t color,
                      uint16_t height = 8u) {
    h2_game_text_metrics_t metrics{};
    if (h2_game_text_measure(game->config.text, text, height, &metrics) == H2_GAME_TEXT_OK) {
        drawText(game, renderer, text, right - metrics.width_px, y, color, height);
    }
}

void drawHeart(pixelroot32::graphics::DrawSurface &surface, int x, int y, uint16_t color) {
    surface.drawFilledRectangle(x, y, 2, 2, color);
    surface.drawFilledRectangle(x + 3, y, 2, 2, color);
    surface.drawFilledRectangle(x - 1, y + 2, 7, 2, color);
    surface.drawFilledRectangle(x, y + 4, 5, 2, color);
    surface.drawFilledRectangle(x + 1, y + 6, 3, 1, color);
}

void drawPlayerProjectile(pixelroot32::graphics::DrawSurface &surface,
                          const PolygonBattleProjectile &projectile) {
    const int x = projectile.x / 1000;
    const int y = projectile.y / 1000;
    if (projectile.vx == 0) {
        surface.drawFilledRectangle(x - 1, y - 6, 3, 12, kCyan);
    } else {
        const int lean = projectile.vx < 0 ? 3 : -3;
        surface.drawLine(x - lean, y + 6, x + lean, y - 6, kCyan);
        surface.drawLine(x - lean + 1, y + 6, x + lean + 1, y - 6, kCyan);
        surface.drawLine(x - lean - 1, y + 6, x + lean - 1, y - 6, kCyan);
    }
}

void drawBattleHud(const h2_polygon_battle *game,
                   pixelroot32::graphics::Renderer &renderer) {
    auto &surface = renderer.getDrawSurface();
    surface.drawFilledRectangle(0, 0, kScreen, 25, kPanel);
    for (uint8_t index = 0; index < 3u; ++index) {
        drawHeart(surface, 8 + index * 10, 7,
                  index < game->life ? kRed : static_cast<uint16_t>(0x3147));
    }
    char wave[20]{};
    const int wave_length = std::snprintf(wave, sizeof(wave), "WAVE %lu",
        static_cast<unsigned long>(game->wave));
    if (wave_length > 0) drawCentered(game, renderer,
        {wave, static_cast<size_t>(std::min<int>(wave_length, sizeof(wave) - 1))}, 8, kWhite);
    char score[16]{};
    const int score_length = std::snprintf(score, sizeof(score), "%05lu",
        static_cast<unsigned long>(game->score));
    if (score_length > 0) drawRightAligned(game, renderer,
        {score, static_cast<size_t>(std::min<int>(score_length, sizeof(score) - 1))}, 232, 8, kYellow);

    char modules[24]{};
    const int modules_length = std::snprintf(modules, sizeof(modules), "S%u P%u R%u D%u",
        game->spread, game->pierce, game->ricochet, game->power);
    if (modules_length > 0) drawText(game, renderer,
        {modules, static_cast<size_t>(std::min<int>(modules_length, sizeof(modules) - 1))},
        8, 230, kMuted);
    static constexpr h2_game_text_span_t kShieldOn = H2_GAME_TEXT_LITERAL("SHIELD ++");
    static constexpr h2_game_text_span_t kShieldOff = H2_GAME_TEXT_LITERAL("SHIELD --");
    drawRightAligned(game, renderer, game->shield ? kShieldOn : kShieldOff,
                     232, 230, kCyan);
}

void drawControlCard(const h2_polygon_battle *game,
                     pixelroot32::graphics::Renderer &renderer,
                     int x,
                     uint16_t fill,
                     uint16_t outline,
                     int kind,
                     h2_game_text_span_t label) {
    auto &surface = renderer.getDrawSurface();
    surface.drawFilledRectangle(x, 133, 52, 38, fill);
    surface.drawRectangle(x, 133, 52, 38, outline);
    if (kind == 0) {
        const ScreenPoint arrow[] = {{x + 17, 152}, {x + 33, 142}, {x + 33, 162}};
        fillPolygon(surface, arrow, 3u, outline);
    } else if (kind == 1) {
        const ScreenPoint arrow[] = {{x + 35, 152}, {x + 19, 142}, {x + 19, 162}};
        fillPolygon(surface, arrow, 3u, outline);
    } else {
        drawDiamond(surface, x + 26, 152, 5, 12, outline);
    }
    h2_game_text_metrics_t metrics{};
    if (h2_game_text_measure(game->config.text, label, 8u, &metrics) == H2_GAME_TEXT_OK) {
        drawText(game, renderer, label, x + (52 - metrics.width_px) / 2, 176, kWhite);
    }
}

void drawReady(const h2_polygon_battle *game,
               pixelroot32::graphics::Renderer &renderer) {
    auto &surface = renderer.getDrawSurface();
    drawCentered(game, renderer, game->config.texts->title, 18, kWhite, 16u);
    drawShipAt(renderer, 120, 84, false, false);
    static constexpr h2_game_text_span_t kLeft = H2_GAME_TEXT_LITERAL("LEFT");
    static constexpr h2_game_text_span_t kRight = H2_GAME_TEXT_LITERAL("RIGHT");
    static constexpr h2_game_text_span_t kFire = H2_GAME_TEXT_LITERAL("FIRE");
    drawControlCard(game, renderer, 27, 0x11aa, 0x4cff, 0, kLeft);
    drawControlCard(game, renderer, 94, 0x2208, 0x573a, 1, kRight);
    drawControlCard(game, renderer, 161, 0x50c5, kRed, 2, kFire);
    surface.drawFilledRectangle(34, 204, 172, 24, kPurple);
    drawCentered(game, renderer, game->config.texts->start, 212, kWhite);
}

void drawResult(const h2_polygon_battle *game,
                pixelroot32::graphics::Renderer &renderer) {
    auto &surface = renderer.getDrawSurface();
    drawShipAt(renderer, 120, 48, false, false);
    drawCentered(game, renderer, game->config.texts->game_over, 88, kRed, 16u);
    surface.drawFilledRectangle(32, 111, 176, 78, 0x10c5);
    surface.drawRectangle(32, 111, 176, 78, 0x31aa);
    drawText(game, renderer, game->config.texts->score, 48, 122, kMuted);
    drawText(game, renderer, game->config.texts->destroyed, 48, 139, kMuted);
    drawText(game, renderer, game->config.texts->max_combo, 48, 156, kMuted);
    static constexpr h2_game_text_span_t kModules = H2_GAME_TEXT_LITERAL("MODULES");
    drawText(game, renderer, kModules, 48, 173, kMuted);

    char value[32]{};
    int length = std::snprintf(value, sizeof(value), "%05lu", static_cast<unsigned long>(game->score));
    if (length > 0) drawRightAligned(game, renderer,
        {value, static_cast<size_t>(length)}, 192, 122, kWhite);
    length = std::snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(game->destroyed_count));
    if (length > 0) drawRightAligned(game, renderer,
        {value, static_cast<size_t>(length)}, 192, 139, kWhite);
    length = std::snprintf(value, sizeof(value), "%u", game->max_combo);
    if (length > 0) drawRightAligned(game, renderer,
        {value, static_cast<size_t>(length)}, 192, 156, kWhite);
    length = std::snprintf(value, sizeof(value), "S%u P%u R%u D%u",
        game->spread, game->pierce, game->ricochet, game->power);
    if (length > 0) drawRightAligned(game, renderer,
        {value, static_cast<size_t>(length)}, 192, 173, kYellow);
    surface.drawFilledRectangle(34, 204, 172, 24, kPurple);
    drawCentered(game, renderer, game->config.texts->retry, 212, kWhite);
}

} // namespace

void PolygonBattleScene::init() {
}

void PolygonBattleScene::update(unsigned long delta_ms) {
    owner->accumulator_us += static_cast<uint64_t>(delta_ms) * 1000u;
    while (owner->accumulator_us >= kStepUs) {
        step();
        owner->accumulator_us -= kStepUs;
    }
}

void PolygonBattleScene::step() {
    auto *game = owner;
    if (game->phase != PolygonBattlePhase::Playing) return;
    game->elapsed_us += kStepUs;
    if (game->invulnerable_frames != 0u) --game->invulnerable_frames;
    if (game->shot_frames != 0u) --game->shot_frames;

    const int direction = static_cast<int>(game->right_down) - static_cast<int>(game->left_down);
    game->player_x = std::clamp(
        game->player_x + stepDistance(direction * 150),
        int32_t{20000},
        int32_t{220000});
    if (game->action_down && game->shot_frames == 0u) firePlayer(game);

    for (auto &enemy : game->enemies) {
        if (!enemy.active) continue;
        if (enemy.destroy_pending) {
            if (enemy.destroy_frames != 0u) --enemy.destroy_frames;
            if (enemy.destroy_frames == 0u) finishEnemy(game, enemy);
            continue;
        }
        enemy.x += stepDistance(enemy.vx);
        if (enemy.x < 17000 || enemy.x > 223000) {
            enemy.vx = static_cast<int16_t>(-enemy.vx);
            enemy.x = std::clamp(enemy.x, int32_t{17000}, int32_t{223000});
        }
        if (enemy.fire_frames != 0u) --enemy.fire_frames;
        if (enemy.fire_frames == 0u) fireEnemy(game, enemy);
    }

    for (auto &projectile : game->projectiles) {
        if (!projectile.active) continue;
        projectile.x += stepDistance(projectile.vx);
        projectile.y += stepDistance(projectile.vy);
        const bool hit_side = projectile.x <= 3000 || projectile.x >= 237000;
        const bool hit_top = projectile.y <= 13000;
        if (hit_side || hit_top) {
            if (projectile.bounces == 0u) {
                projectile.active = false;
                continue;
            }
            --projectile.bounces;
            if (hit_side) projectile.vx = static_cast<int16_t>(-projectile.vx);
            if (hit_top) projectile.vy = static_cast<int16_t>(-projectile.vy);
            projectile.x = std::clamp(
                projectile.x, int32_t{3000}, int32_t{237000});
            projectile.y = std::max(projectile.y, int32_t{13000});
        }
        if (projectile.y > 240000) {
            projectile.active = false;
            continue;
        }
        for (auto &enemy : game->enemies) {
            if (!projectile.active || !enemy.active || enemy.destroy_pending || alreadyHit(projectile, enemy.id)) continue;
            if (std::abs(projectile.x - enemy.x) <= 14000 && std::abs(projectile.y - enemy.y) <= 14000) {
                damageEnemy(game, enemy, projectile);
            }
        }
    }

    for (auto &shot : game->enemy_projectiles) {
        if (!shot.active) continue;
        shot.x += stepDistance(shot.vx);
        shot.y += stepDistance(shot.vy);
        if (shot.y > 244000 || shot.x < -4000 || shot.x > 244000) {
            shot.active = false;
        } else if (std::abs(shot.x - game->player_x) <= 9000 && std::abs(shot.y - kPlayerY) <= 9000) {
            shot.active = false;
            damagePlayer(game);
        }
    }
    if (game->phase != PolygonBattlePhase::Playing) return;

    for (auto &pickup : game->pickups) {
        if (!pickup.active) continue;
        pickup.y += stepDistance(55);
        if (pickup.y > 244000) pickup.active = false;
        else if (std::abs(pickup.x - game->player_x) <= 15000 && pickup.y >= 195000) collectPickup(game, pickup);
    }

    if (!anyEnemy(game)) {
        if (game->wave_pause_frames == 0u) {
            game->wave_pause_frames = 54u;
            notify(game, {H2_POLYGON_BATTLE_EVENT_WAVE_CLEARED, 0u,
                          H2_POLYGON_BATTLE_SHAPE_CIRCLE, 0u, 0u, game->wave});
        } else if (--game->wave_pause_frames == 0u) {
            if (game->wave != 0xffffffffu) ++game->wave;
            spawnWave(game, game->wave);
        }
    }
}

void PolygonBattleScene::draw(pixelroot32::graphics::Renderer &renderer) {
    auto &surface = renderer.getDrawSurface();
    surface.drawFilledRectangle(0, 0, kScreen, kScreen, kBackground);
    for (int index = 0; index < 24; ++index) {
        const uint32_t elapsed_ms = elapsedMs(owner);
        const int x =
            (index * 71 + static_cast<int>(elapsed_ms / 53u)) % kScreen;
        const int y =
            (index * 43 + static_cast<int>(elapsed_ms / 97u)) % kScreen;
        surface.drawFilledRectangle(x, y, index % 7 == 0 ? 2 : 1, 1,
                                    index % 3 == 0 ? kWhite : 0x4208);
    }

    if (owner->phase == PolygonBattlePhase::Ready) {
        drawReady(owner, renderer);
    } else if (owner->phase == PolygonBattlePhase::GameOver) {
        drawResult(owner, renderer);
    } else {
        drawBattleHud(owner, renderer);
        for (const auto &enemy : owner->enemies) if (enemy.active) drawEnemy(renderer, enemy);
        for (const auto &projectile : owner->projectiles) if (projectile.active) {
            drawPlayerProjectile(surface, projectile);
        }
        for (const auto &shot : owner->enemy_projectiles) if (shot.active) {
            drawEnemyProjectile(surface, shot);
        }
        for (const auto &pickup : owner->pickups) if (pickup.active) {
            drawPickup(owner, renderer, pickup);
        }
        drawShip(renderer, owner);
    }
}

extern "C" const h2_polygon_battle_texts_t *h2_polygon_battle_english_texts(void) {
    return &kEnglishTexts;
}

extern "C" int h2_polygon_battle_create(
    const h2_polygon_battle_config_t *config,
    h2_polygon_battle_t **out_game) {
    if (out_game == nullptr) return H2_POLYGON_BATTLE_ERR_INVALID_ARGUMENT;
    *out_game = nullptr;
    if (config == nullptr || config->text == nullptr ||
        config->text->vtable == nullptr ||
        config->text->vtable->measure == nullptr ||
        config->text->vtable->draw == nullptr ||
        config->texts == nullptr ||
        !validText(config->texts->title) ||
        !validText(config->texts->start) ||
        !validText(config->texts->life) ||
        !validText(config->texts->score) ||
        !validText(config->texts->wave) ||
        !validText(config->texts->spread) ||
        !validText(config->texts->pierce) ||
        !validText(config->texts->ricochet) ||
        !validText(config->texts->power) ||
        !validText(config->texts->game_over) ||
        !validText(config->texts->destroyed) ||
        !validText(config->texts->max_combo) ||
        !validText(config->texts->retry)) {
        return H2_POLYGON_BATTLE_ERR_INVALID_ARGUMENT;
    }
    *out_game = new (std::nothrow) h2_polygon_battle(*config);
    return *out_game == nullptr ? H2_POLYGON_BATTLE_ERR_NO_MEMORY : H2_POLYGON_BATTLE_OK;
}

extern "C" h2_game_scene_t *h2_polygon_battle_scene(h2_polygon_battle_t *game) {
    return game == nullptr ? nullptr : reinterpret_cast<h2_game_scene_t *>(&game->scene);
}

extern "C" void h2_polygon_battle_handle_input(
    h2_polygon_battle_t *game,
    const h2_game_input_event_t *event) {
    if (game == nullptr || event == nullptr) return;
    const bool down = event->type == H2_GAME_INPUT_BUTTON_DOWN;
    const bool up = event->type == H2_GAME_INPUT_BUTTON_UP;
    if (event->button == H2_POLYGON_BATTLE_BUTTON_LEFT) {
        if (down) game->left_down = true;
        if (up) game->left_down = false;
    } else if (event->button == H2_POLYGON_BATTLE_BUTTON_RIGHT) {
        if (down) game->right_down = true;
        if (up) game->right_down = false;
    } else if (event->button == H2_POLYGON_BATTLE_BUTTON_ACTION) {
        if (down && game->phase != PolygonBattlePhase::Playing) {
            beginGame(game);
            game->action_down = true;
        } else if (down && !game->action_down) {
            game->action_down = true;
            if (game->shot_frames == 0u) firePlayer(game);
        }
        if (up) game->action_down = false;
    }
}

extern "C" void h2_polygon_battle_reset(h2_polygon_battle_t *game) {
    if (game == nullptr) return;
    clearActors(game);
    game->phase = PolygonBattlePhase::Ready;
    game->accumulator_us = 0u;
    game->wave = 0;
    game->score = 0;
    game->destroyed_count = 0;
    game->elapsed_us = 0;
    game->next_enemy_id = 1;
    game->combo = 0;
    game->max_combo = 0;
    game->shot_frames = 0;
    game->invulnerable_frames = 0;
    game->wave_pause_frames = 0;
    game->life = 3;
    game->spread = 1;
    game->pierce = 1;
    game->ricochet = 0;
    game->power = 1;
    game->shield = false;
    game->left_down = false;
    game->right_down = false;
    game->action_down = false;
    game->player_x = 120000;
    game->random_state = nextRandom(game);
}

extern "C" int h2_polygon_battle_get_result(
    const h2_polygon_battle_t *game,
    h2_polygon_battle_result_t *out_result) {
    if (game == nullptr || out_result == nullptr) return H2_POLYGON_BATTLE_ERR_INVALID_ARGUMENT;
    *out_result = {game->score, game->destroyed_count, elapsedMs(game),
                   game->max_combo, game->wave, game->life, game->spread, game->pierce,
                   game->ricochet, game->power};
    return H2_POLYGON_BATTLE_OK;
}

extern "C" void h2_polygon_battle_destroy(h2_polygon_battle_t *game) { delete game; }
