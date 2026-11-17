#include "h2_fruit_slice_game.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Radii for a 480-pixel short side. */
static const float k_radius[H2_FRUIT_SLICE_KIND_COUNT] = {46.0f, 34.0f, 32.0f,
                                                          28.0f, 30.0f, 30.0f};

#define STEP_MAX_MS 16u
#define MISS_LIFE_MS 1200u
#define SPLAT_LIFE_MS 900u
#define POPUP_LIFE_MS 900u

static uint32_t random_next(h2_fruit_slice_game_t* game) {
  uint32_t x = game->rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  game->rng = x;
  return x;
}

/* Uniform in [0, 1). */
static float random_unit(h2_fruit_slice_game_t* game) {
  return (float)(random_next(game) >> 8) * (1.0f / 16777216.0f);
}

static float random_range(h2_fruit_slice_game_t* game, float low, float high) {
  return low + (high - low) * random_unit(game);
}

float h2_fruit_slice_game_radius(const h2_fruit_slice_game_t* game,
                                 h2_fruit_slice_kind_t kind) {
  if ((unsigned)kind >= H2_FRUIT_SLICE_KIND_COUNT) kind = H2_FRUIT_SLICE_ORANGE;
  return k_radius[kind] * game->unit;
}

uint16_t h2_fruit_slice_game_juice(h2_fruit_slice_kind_t kind) {
  switch (kind) {
    case H2_FRUIT_SLICE_WATERMELON:
      return H2_FRUIT_SLICE_RGB(232, 38, 62);
    case H2_FRUIT_SLICE_ORANGE:
      return H2_FRUIT_SLICE_RGB(255, 152, 24);
    case H2_FRUIT_SLICE_APPLE:
      return H2_FRUIT_SLICE_RGB(250, 226, 150);
    case H2_FRUIT_SLICE_KIWI:
      return H2_FRUIT_SLICE_RGB(132, 204, 56);
    case H2_FRUIT_SLICE_LEMON:
      return H2_FRUIT_SLICE_RGB(255, 232, 72);
    default:
      return H2_FRUIT_SLICE_RGB(96, 96, 96);
  }
}

static void clear_world(h2_fruit_slice_game_t* game) {
  memset(game->fruits, 0, sizeof(game->fruits));
  memset(game->halves, 0, sizeof(game->halves));
  memset(game->particles, 0, sizeof(game->particles));
  memset(game->splats, 0, sizeof(game->splats));
  memset(game->popups, 0, sizeof(game->popups));
  memset(game->misses, 0, sizeof(game->misses));
  game->pending_spawns = 0;
  game->wave_has_bomb = false;
  game->stroke_slices = 0;
}

static h2_fruit_slice_fruit_t* add_fruit(h2_fruit_slice_game_t* game,
                                         h2_fruit_slice_kind_t kind) {
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i) {
    h2_fruit_slice_fruit_t* fruit = &game->fruits[i];
    if (fruit->active) continue;
    memset(fruit, 0, sizeof(*fruit));
    fruit->active = true;
    fruit->kind = (uint8_t)kind;
    fruit->radius = h2_fruit_slice_game_radius(game, kind);
    return fruit;
  }
  return NULL;
}

static void add_menu_fruit(h2_fruit_slice_game_t* game,
                           h2_fruit_slice_kind_t kind, float y) {
  h2_fruit_slice_fruit_t* fruit = add_fruit(game, kind);
  if (fruit == NULL) return;
  fruit->menu = true;
  /* The menu target is larger than in play so it is easy to hit. */
  fruit->radius *= H2_FRUIT_SLICE_MENU_SCALE;
  fruit->body.x = (float)game->width * 0.5f;
  fruit->body.y = y;
  fruit->body.spin = 0.7f;
}

static bool menu_present(const h2_fruit_slice_game_t* game) {
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i)
    if (game->fruits[i].active && game->fruits[i].menu) return true;
  return false;
}

static void add_popup(h2_fruit_slice_game_t* game, const char* text, float x,
                      float y, uint16_t color) {
  h2_fruit_slice_popup_t* slot = &game->popups[0];
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_POPUPS; ++i) {
    h2_fruit_slice_popup_t* popup = &game->popups[i];
    if (!popup->active) {
      slot = popup;
      break;
    }
    if (popup->age_ms > slot->age_ms) slot = popup;
  }
  memset(slot, 0, sizeof(*slot));
  slot->active = true;
  slot->x = x;
  slot->y = y;
  slot->color = color;
  slot->life_ms = POPUP_LIFE_MS;
  (void)snprintf(slot->text, sizeof(slot->text), "%s", text);
}

static void enter_title(h2_fruit_slice_game_t* game) {
  clear_world(game);
  game->phase = H2_FRUIT_SLICE_PHASE_TITLE;
  game->phase_started_ms = game->time_ms;
  add_menu_fruit(game, H2_FRUIT_SLICE_WATERMELON, (float)game->height * 0.60f);
}

static void enter_playing(h2_fruit_slice_game_t* game) {
  clear_world(game);
  game->phase = H2_FRUIT_SLICE_PHASE_PLAYING;
  game->phase_started_ms = game->time_ms;
  game->score = 0;
  game->lives = H2_FRUIT_SLICE_LIVES;
  game->waves = 0;
  game->new_best = false;
  game->next_wave_ms = game->time_ms + 700u;
}

static void enter_game_over(h2_fruit_slice_game_t* game) {
  game->phase = H2_FRUIT_SLICE_PHASE_GAME_OVER;
  game->phase_started_ms = game->time_ms;
  game->pending_spawns = 0;
  game->stroke_slices = 0;
  if (game->score > game->best) {
    game->best = game->score;
    game->best_changed = true;
    game->new_best = true;
  }
}

void h2_fruit_slice_game_init(h2_fruit_slice_game_t* game, int32_t width,
                              int32_t height, uint32_t seed, uint32_t best) {
  memset(game, 0, sizeof(*game));
  game->width = width > 0 ? width : 1;
  game->height = height > 0 ? height : 1;
  const int32_t short_side =
      game->width < game->height ? game->width : game->height;
  game->unit = (float)short_side / 480.0f;
  /* About one second from the bottom edge to an apex 3/4 of the way up. */
  game->gravity = 1.5f * (float)game->height;
  game->rng = seed != 0 ? seed : 0x9E3779B9u;
  game->best = best;
  enter_title(game);
}

bool h2_fruit_slice_game_take_best(h2_fruit_slice_game_t* game,
                                   uint32_t* out_best) {
  if (!game->best_changed) return false;
  game->best_changed = false;
  if (out_best != NULL) *out_best = game->best;
  return true;
}

static void spawn_thrown(h2_fruit_slice_game_t* game,
                         h2_fruit_slice_kind_t kind) {
  h2_fruit_slice_fruit_t* fruit = add_fruit(game, kind);
  if (fruit == NULL) return;
  const float w = (float)game->width, h = (float)game->height;
  const float x0 = random_range(game, 0.18f * w, 0.82f * w);
  const float y0 = h + fruit->radius;
  const float apex = random_range(game, 0.10f * h, 0.38f * h);
  const float rise = y0 - apex;
  const float vy = -sqrtf(2.0f * game->gravity * rise);
  const float flight_s = 2.0f * (-vy / game->gravity);
  const float target = random_range(game, 0.15f * w, 0.85f * w);
  fruit->body.x = x0;
  fruit->body.y = y0;
  fruit->body.vx = (target - x0) / flight_s;
  fruit->body.vy = vy;
  fruit->body.angle = random_range(game, 0.0f, 2.0f * H2_FRUIT_SLICE_PI);
  fruit->body.spin = random_range(game, -3.0f, 3.0f);
}

static void update_spawns(h2_fruit_slice_game_t* game) {
  if (game->phase != H2_FRUIT_SLICE_PHASE_PLAYING) return;
  if (game->pending_spawns == 0 && game->time_ms >= game->next_wave_ms) {
    ++game->waves;
    uint32_t spread = 1u + game->score / 10u + game->waves / 8u;
    if (spread > 4u) spread = 4u;
    const uint32_t count = 1u + random_next(game) % (spread + 1u);
    float bomb_chance = 0.0f;
    if (game->waves > 2u) {
      bomb_chance = 0.10f + (float)game->score * 0.004f;
      if (bomb_chance > 0.30f) bomb_chance = 0.30f;
    }
    game->wave_has_bomb = random_unit(game) < bomb_chance;
    game->pending_spawns = count + (game->wave_has_bomb ? 1u : 0u);
    game->next_spawn_ms = game->time_ms;
  }
  while (game->pending_spawns > 0 && game->time_ms >= game->next_spawn_ms) {
    h2_fruit_slice_kind_t kind =
        (h2_fruit_slice_kind_t)(random_next(game) % H2_FRUIT_SLICE_FRUIT_KINDS);
    if (game->wave_has_bomb &&
        (game->pending_spawns == 1u || random_unit(game) < 0.4f)) {
      kind = H2_FRUIT_SLICE_BOMB;
      game->wave_has_bomb = false;
    }
    spawn_thrown(game, kind);
    --game->pending_spawns;
    game->next_spawn_ms = game->time_ms + 120u + random_next(game) % 160u;
    if (game->pending_spawns == 0) {
      uint32_t interval =
          game->score * 12u < 900u ? 1800u - game->score * 12u : 900u;
      game->next_wave_ms = game->time_ms + interval + random_next(game) % 400u;
    }
  }
}

static void add_particle(h2_fruit_slice_game_t* game, float x, float y,
                         float vx, float vy, float size, uint16_t color,
                         uint16_t life_ms) {
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_PARTICLES; ++i) {
    h2_fruit_slice_particle_t* p = &game->particles[i];
    if (p->active) continue;
    *p = (h2_fruit_slice_particle_t){.active = true,
                                     .x = x,
                                     .y = y,
                                     .vx = vx,
                                     .vy = vy,
                                     .size = size,
                                     .color = color,
                                     .life_ms = life_ms};
    return;
  }
}

static void add_splat(h2_fruit_slice_game_t* game, float x, float y,
                      float radius, uint16_t color) {
  h2_fruit_slice_splat_t* slot = &game->splats[0];
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_SPLATS; ++i) {
    h2_fruit_slice_splat_t* splat = &game->splats[i];
    if (!splat->active) {
      slot = splat;
      break;
    }
    if (splat->age_ms > slot->age_ms) slot = splat;
  }
  *slot = (h2_fruit_slice_splat_t){.active = true,
                                   .x = x,
                                   .y = y,
                                   .radius = radius,
                                   .color = color,
                                   .life_ms = SPLAT_LIFE_MS};
}

static void add_half(h2_fruit_slice_game_t* game,
                     const h2_fruit_slice_fruit_t* fruit, float nx, float ny,
                     float ux, float uy) {
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_HALVES; ++i) {
    h2_fruit_slice_half_t* half = &game->halves[i];
    if (half->active) continue;
    const float separate = 110.0f * game->unit;
    half->active = true;
    half->kind = fruit->kind;
    half->radius = fruit->radius;
    half->body = fruit->body;
    half->body.vx += nx * separate + ux * 40.0f * game->unit;
    half->body.vy += ny * separate + uy * 40.0f * game->unit;
    half->body.angle = atan2f(ny, nx);
    half->body.spin = random_range(game, -4.0f, 4.0f);
    return;
  }
}

static void burst(h2_fruit_slice_game_t* game, float x, float y, float ux,
                  float uy, unsigned count, uint16_t color, float speed,
                  float size) {
  for (unsigned i = 0; i < count; ++i) {
    const float angle = random_range(game, 0.0f, 2.0f * H2_FRUIT_SLICE_PI);
    const float v = random_range(game, 0.3f, 1.0f) * speed * game->unit;
    add_particle(game, x, y, cosf(angle) * v + ux * v * 0.6f,
                 sinf(angle) * v + uy * v * 0.6f - 60.0f * game->unit,
                 random_range(game, 0.5f, 1.0f) * size * game->unit, color,
                 (uint16_t)(350u + random_next(game) % 300u));
  }
}

static void slice_fruit(h2_fruit_slice_game_t* game, unsigned index, float ux,
                        float uy) {
  const h2_fruit_slice_fruit_t fruit = game->fruits[index];
  game->fruits[index].active = false;
  const float nx = -uy, ny = ux;
  const h2_fruit_slice_kind_t kind = (h2_fruit_slice_kind_t)fruit.kind;

  if (kind == H2_FRUIT_SLICE_BOMB) {
    burst(game, fruit.body.x, fruit.body.y, ux, uy, 18,
          H2_FRUIT_SLICE_RGB(255, 170, 40), 360.0f, 6.0f);
    burst(game, fruit.body.x, fruit.body.y, ux, uy, 12,
          H2_FRUIT_SLICE_RGB(70, 70, 76), 220.0f, 8.0f);
    game->flash_ms = H2_FRUIT_SLICE_FLASH_MS;
    game->flash_x = fruit.body.x;
    game->flash_y = fruit.body.y;
    if (game->phase == H2_FRUIT_SLICE_PHASE_PLAYING) enter_game_over(game);
    return;
  }

  if (fruit.menu) enter_playing(game);
  add_half(game, &fruit, nx, ny, ux, uy);
  add_half(game, &fruit, -nx, -ny, ux, uy);
  const uint16_t juice = h2_fruit_slice_game_juice(kind);
  burst(game, fruit.body.x, fruit.body.y, ux, uy, 12, juice, 240.0f, 5.0f);
  add_splat(game, fruit.body.x, fruit.body.y, fruit.radius * 1.15f, juice);

  if (fruit.menu || game->phase != H2_FRUIT_SLICE_PHASE_PLAYING) return;
  ++game->score;
  ++game->sliced_total;
  ++game->stroke_slices;
  game->stroke_last_slice_ms = game->time_ms;
  game->stroke_last_x = fruit.body.x;
  game->stroke_last_y = fruit.body.y;
}

static void finish_combo(h2_fruit_slice_game_t* game) {
  const uint32_t slices = game->stroke_slices;
  game->stroke_slices = 0;
  if (slices < H2_FRUIT_SLICE_COMBO_MIN ||
      game->phase != H2_FRUIT_SLICE_PHASE_PLAYING)
    return;
  game->score += slices;
  char text[H2_FRUIT_SLICE_POPUP_TEXT_MAX];
  (void)snprintf(text, sizeof(text), "COMBO +%u", (unsigned)slices);
  add_popup(game, text, game->stroke_last_x, game->stroke_last_y,
            H2_FRUIT_SLICE_RGB(255, 214, 64));
}

/* Distance from (px, py) to the segment, squared. */
static float segment_distance2(float x0, float y0, float x1, float y1, float px,
                               float py) {
  const float dx = x1 - x0, dy = y1 - y0;
  const float length2 = dx * dx + dy * dy;
  float t = length2 > 0.0f ? ((px - x0) * dx + (py - y0) * dy) / length2 : 0.0f;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  const float ex = x0 + t * dx - px, ey = y0 + t * dy - py;
  return ex * ex + ey * ey;
}

static void slice_segment(h2_fruit_slice_game_t* game, float x0, float y0,
                          float x1, float y1) {
  const float dx = x1 - x0, dy = y1 - y0;
  const float length2 = dx * dx + dy * dy;
  const float minimum = 2.0f * game->unit;
  if (length2 < minimum * minimum) return;
  const float length = sqrtf(length2);
  const float ux = dx / length, uy = dy / length;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i) {
    const h2_fruit_slice_fruit_t* fruit = &game->fruits[i];
    if (!fruit->active) continue;
    const float reach = fruit->radius * 0.95f;
    if (segment_distance2(x0, y0, x1, y1, fruit->body.x, fruit->body.y) <=
        reach * reach)
      slice_fruit(game, i, ux, uy);
  }
}

static void add_trail_point(h2_fruit_slice_game_t* game, float x, float y) {
  if (game->trail_count == H2_FRUIT_SLICE_TRAIL_MAX) {
    memmove(&game->trail[0], &game->trail[1],
            sizeof(game->trail[0]) * (H2_FRUIT_SLICE_TRAIL_MAX - 1u));
    --game->trail_count;
  }
  game->trail[game->trail_count++] =
      (h2_fruit_slice_trail_point_t){x, y, game->time_ms};
}

void h2_fruit_slice_game_pointer(h2_fruit_slice_game_t* game,
                                 h2_fruit_slice_pointer_t kind, float x,
                                 float y) {
  switch (kind) {
    case H2_FRUIT_SLICE_POINTER_DOWN:
      finish_combo(game);
      game->pointer_down = true;
      game->has_last = true;
      game->last_x = x;
      game->last_y = y;
      game->trail_count = 0;
      add_trail_point(game, x, y);
      break;
    case H2_FRUIT_SLICE_POINTER_MOVE:
      if (!game->pointer_down || !game->has_last) {
        h2_fruit_slice_game_pointer(game, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
        break;
      }
      add_trail_point(game, x, y);
      slice_segment(game, game->last_x, game->last_y, x, y);
      game->last_x = x;
      game->last_y = y;
      break;
    case H2_FRUIT_SLICE_POINTER_UP:
    case H2_FRUIT_SLICE_POINTER_CANCEL:
    default:
      if (kind == H2_FRUIT_SLICE_POINTER_UP && game->pointer_down &&
          game->has_last && (x != game->last_x || y != game->last_y)) {
        add_trail_point(game, x, y);
        slice_segment(game, game->last_x, game->last_y, x, y);
      }
      game->pointer_down = false;
      game->has_last = false;
      finish_combo(game);
      break;
  }
}

static void advance_body(h2_fruit_slice_body_t* body, float gravity, float dt) {
  body->vy += gravity * dt;
  body->x += body->vx * dt;
  body->y += body->vy * dt;
  body->angle += body->spin * dt;
  if (body->angle > 2.0f * H2_FRUIT_SLICE_PI)
    body->angle -= 2.0f * H2_FRUIT_SLICE_PI;
  if (body->angle < 0.0f) body->angle += 2.0f * H2_FRUIT_SLICE_PI;
}

static void record_miss(h2_fruit_slice_game_t* game, float x) {
  ++game->missed_total;
  if (game->lives > 0) --game->lives;
  h2_fruit_slice_miss_t* slot = &game->misses[0];
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_MISSES; ++i) {
    h2_fruit_slice_miss_t* miss = &game->misses[i];
    if (!miss->active) {
      slot = miss;
      break;
    }
    if (miss->age_ms > slot->age_ms) slot = miss;
  }
  const float margin = 28.0f * game->unit;
  if (x < margin) x = margin;
  if (x > (float)game->width - margin) x = (float)game->width - margin;
  *slot = (h2_fruit_slice_miss_t){.active = true, .x = x};
  if (game->lives == 0) enter_game_over(game);
}

static uint16_t age(uint16_t value, uint32_t ms) {
  const uint32_t next = (uint32_t)value + ms;
  return next > 0xFFFFu ? 0xFFFFu : (uint16_t)next;
}

static void step_once(h2_fruit_slice_game_t* game, uint32_t ms) {
  game->time_ms += ms;
  const float dt = (float)ms / 1000.0f;
  const float w = (float)game->width, h = (float)game->height;

  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i) {
    h2_fruit_slice_fruit_t* fruit = &game->fruits[i];
    if (!fruit->active) continue;
    if (fruit->menu) {
      fruit->body.angle += fruit->body.spin * dt;
      continue;
    }
    advance_body(&fruit->body, game->gravity, dt);
    const float r = fruit->radius;
    if (fruit->body.x < -2.0f * r || fruit->body.x > w + 2.0f * r) {
      fruit->active = false;
    } else if (fruit->body.vy > 0.0f && fruit->body.y - r > h) {
      fruit->active = false;
      if (game->phase == H2_FRUIT_SLICE_PHASE_PLAYING &&
          fruit->kind != H2_FRUIT_SLICE_BOMB)
        record_miss(game, fruit->body.x);
    }
  }
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_HALVES; ++i) {
    h2_fruit_slice_half_t* half = &game->halves[i];
    if (!half->active) continue;
    advance_body(&half->body, game->gravity, dt);
    if (half->body.y - half->radius > h) half->active = false;
  }
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_PARTICLES; ++i) {
    h2_fruit_slice_particle_t* p = &game->particles[i];
    if (!p->active) continue;
    p->vy += game->gravity * 0.8f * dt;
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    p->age_ms = age(p->age_ms, ms);
    if (p->age_ms >= p->life_ms || p->y - p->size > h) p->active = false;
  }
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_SPLATS; ++i) {
    h2_fruit_slice_splat_t* splat = &game->splats[i];
    if (!splat->active) continue;
    splat->age_ms = age(splat->age_ms, ms);
    if (splat->age_ms >= splat->life_ms) splat->active = false;
  }
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_POPUPS; ++i) {
    h2_fruit_slice_popup_t* popup = &game->popups[i];
    if (!popup->active) continue;
    popup->y -= 40.0f * game->unit * dt;
    popup->age_ms = age(popup->age_ms, ms);
    if (popup->age_ms >= popup->life_ms) popup->active = false;
  }
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_MISSES; ++i) {
    h2_fruit_slice_miss_t* miss = &game->misses[i];
    if (!miss->active) continue;
    miss->age_ms = age(miss->age_ms, ms);
    if (miss->age_ms >= MISS_LIFE_MS) miss->active = false;
  }
  game->flash_ms = game->flash_ms > ms ? game->flash_ms - ms : 0u;

  uint32_t expired = 0;
  while (expired < game->trail_count &&
         game->trail[expired].time_ms + H2_FRUIT_SLICE_TRAIL_LIFE_MS <
             game->time_ms)
    ++expired;
  if (expired != 0) {
    memmove(&game->trail[0], &game->trail[expired],
            sizeof(game->trail[0]) * (game->trail_count - expired));
    game->trail_count -= expired;
  }

  if (game->stroke_slices != 0 && game->time_ms - game->stroke_last_slice_ms >
                                      H2_FRUIT_SLICE_COMBO_WINDOW_MS)
    finish_combo(game);

  if (game->phase == H2_FRUIT_SLICE_PHASE_GAME_OVER &&
      game->time_ms - game->phase_started_ms >=
          H2_FRUIT_SLICE_RESTART_DELAY_MS &&
      !menu_present(game))
    add_menu_fruit(game, H2_FRUIT_SLICE_ORANGE, h * 0.72f);

  update_spawns(game);
}

void h2_fruit_slice_game_step(h2_fruit_slice_game_t* game, uint32_t dt_ms) {
  while (dt_ms > 0) {
    const uint32_t ms = dt_ms < STEP_MAX_MS ? dt_ms : STEP_MAX_MS;
    step_once(game, ms);
    dt_ms -= ms;
  }
}
