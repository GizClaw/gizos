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

static const h2_fruit_slice_weapon_t k_weapons[H2_FRUIT_SLICE_WEAPONS] = {
    {"CLASSIC KNIFE", "STANDARD CUT", 0u, 0.0f, 7.0f,
     H2_FRUIT_SLICE_RGB(110, 214, 255)},
    {"JADE KATANA", "+10 CUT REACH", 40u, 10.0f, 11.0f,
     H2_FRUIT_SLICE_RGB(100, 255, 152)},
    {"FLAME AXE", "+20 CUT REACH", 90u, 20.0f, 17.0f,
     H2_FRUIT_SLICE_RGB(255, 130, 40)},
};
static const h2_fruit_slice_task_t k_tasks[H2_FRUIT_SLICE_TASKS] = {
    {"SLICE 10 FRUIT", 10u, 15u, true},
    {"MAKE 3 COMBOS", 3u, 25u, true},
    {"SCORE 20 IN RUN", 20u, 40u, false},
};

const h2_fruit_slice_weapon_t* h2_fruit_slice_weapon(unsigned id) {
  return &k_weapons[id < H2_FRUIT_SLICE_WEAPONS ? id : 0u];
}

const h2_fruit_slice_task_t* h2_fruit_slice_task(unsigned id) {
  return &k_tasks[id < H2_FRUIT_SLICE_TASKS ? id : 0u];
}

uint32_t h2_fruit_slice_task_progress(const h2_fruit_slice_progress_t* p,
                                      unsigned id) {
  if (id == 0u) return p->fruit_credit;
  if (id == 1u) return p->combo_credit;
  return id == 2u && p->score_task != 0u ? k_tasks[2].target : 0u;
}

void h2_fruit_slice_progress_encode(
    const h2_fruit_slice_progress_t* p,
    uint8_t out[H2_FRUIT_SLICE_PROGRESS_BYTES]) {
  const uint32_t fields[] = {
      0x31505346u,   1u,       p->coins,   p->fruit_credit, p->combo_credit,
      p->score_task, p->owned, p->equipped};
  for (unsigned i = 0; i < 8u; ++i)
    for (unsigned byte = 0; byte < 4u; ++byte)
      out[i * 4u + byte] = (uint8_t)(fields[i] >> (byte * 8u));
}

bool h2_fruit_slice_progress_decode(h2_fruit_slice_progress_t* p,
                                    const void* data, size_t size) {
  if (p == NULL || data == NULL || size != H2_FRUIT_SLICE_PROGRESS_BYTES)
    return false;
  const uint8_t* bytes = data;
  uint32_t f[8] = {0};
  for (unsigned i = 0; i < 8u; ++i)
    for (unsigned byte = 0; byte < 4u; ++byte)
      f[i] |= (uint32_t)bytes[i * 4u + byte] << (byte * 8u);
  if (f[0] != 0x31505346u || f[1] != 1u ||
      f[2] > H2_FRUIT_SLICE_PROGRESS_LIMIT ||
      f[3] > H2_FRUIT_SLICE_PROGRESS_LIMIT ||
      f[4] > H2_FRUIT_SLICE_PROGRESS_LIMIT || f[5] > 2u || (f[6] & 1u) == 0u ||
      (f[6] & ~7u) != 0u || f[7] >= H2_FRUIT_SLICE_WEAPONS ||
      (f[6] & (1u << f[7])) == 0u)
    return false;
  *p = (h2_fruit_slice_progress_t){f[2], f[3], f[4], f[5], f[6], f[7]};
  return true;
}

static void progress_changed(h2_fruit_slice_game_t* game) {
  game->progress_dirty = true;
  ++game->progress_revision;
}

static void score_task_update(h2_fruit_slice_game_t* game) {
  if (game->score >= k_tasks[2].target && game->progress.score_task == 0u) {
    game->progress.score_task = 1u;
    progress_changed(game);
  }
}

void h2_fruit_slice_game_back_rect(int32_t width, int32_t height,
                                   h2_fruit_slice_rect_t* out) {
  const float w = (float)width, h = (float)height;
  const float unit = (float)(width < height ? width : height) / 480.0f;
  const float margin = fmaxf(8.0f, 12.0f * unit);
  const float footer_h = fmaxf(32.0f, 60.0f * unit);
  *out = (h2_fruit_slice_rect_t){
      width <= 240 ? margin : w * 0.25f, h - margin - footer_h,
      width <= 240 ? w - margin : w * 0.75f, h - margin};
}

bool h2_fruit_slice_game_button_rect(const h2_fruit_slice_game_t* game,
                                     uint32_t button,
                                     h2_fruit_slice_rect_t* out) {
  const float w = (float)game->width, h = (float)game->height;
  const float margin = fmaxf(8.0f, 12.0f * game->unit);
  const float gap = fmaxf(6.0f, 8.0f * game->unit);
  const float footer_h = fmaxf(32.0f, 60.0f * game->unit);
  const float footer_y = h - margin - footer_h;
  if (button == H2_FRUIT_SLICE_BUTTON_LANGUAGE &&
      game->phase == H2_FRUIT_SLICE_PHASE_TITLE) {
    *out = (h2_fruit_slice_rect_t){margin, margin,
                                   margin + fmaxf(42.0f, 84.0f * game->unit),
                                   margin + fmaxf(28.0f, 36.0f * game->unit)};
    return true;
  }
  if (game->phase == H2_FRUIT_SLICE_PHASE_TITLE ||
      game->phase == H2_FRUIT_SLICE_PHASE_GAME_OVER) {
    if (button == H2_FRUIT_SLICE_BUTTON_START) {
      const float button_w =
          w * (h < 220.0f && game->phase == H2_FRUIT_SLICE_PHASE_TITLE ? 0.54f
                                                                       : 0.64f);
      const float button_h =
          fmaxf(h < 280.0f ? 32.0f : 40.0f, 56.0f * game->unit);
      const bool beside_fruit =
          h < 220.0f || game->phase == H2_FRUIT_SLICE_PHASE_GAME_OVER;
      const float left = h < 220.0f && game->phase == H2_FRUIT_SLICE_PHASE_TITLE
                             ? w * 0.40f
                         : beside_fruit ? w * 0.30f
                                        : (w - button_w) * 0.5f;
      *out = (h2_fruit_slice_rect_t){left, footer_y - gap - button_h,
                                     left + button_w, footer_y - gap};
      return true;
    }
    if (game->phase == H2_FRUIT_SLICE_PHASE_TITLE &&
        (button == H2_FRUIT_SLICE_BUTTON_SFX ||
         button == H2_FRUIT_SLICE_BUTTON_MUSIC)) {
      const float sound_w = fmaxf(42.0f, 84.0f * game->unit);
      const float sound_h = fmaxf(28.0f, 36.0f * game->unit);
      const float right =
          w - margin -
          (button == H2_FRUIT_SLICE_BUTTON_SFX ? sound_w + gap : 0.0f);
      *out = (h2_fruit_slice_rect_t){right - sound_w, margin, right,
                                     margin + sound_h};
      return true;
    }
    if (button != H2_FRUIT_SLICE_BUTTON_TASKS &&
        button != H2_FRUIT_SLICE_BUTTON_SHOP &&
        button != H2_FRUIT_SLICE_BUTTON_CARDS)
      return false;
    const unsigned column = button == H2_FRUIT_SLICE_BUTTON_TASKS   ? 0u
                            : button == H2_FRUIT_SLICE_BUTTON_CARDS ? 1u
                                                                    : 2u;
    const float button_w = (w - 2.0f * margin - 2.0f * gap) / 3.0f;
    const float left = margin + (button_w + gap) * (float)column;
    *out = (h2_fruit_slice_rect_t){left, footer_y, left + button_w, h - margin};
    return true;
  }
  const bool tasks = game->phase == H2_FRUIT_SLICE_PHASE_TASKS;
  if (!tasks && game->phase != H2_FRUIT_SLICE_PHASE_SHOP &&
      game->phase != H2_FRUIT_SLICE_PHASE_CARDS)
    return false;
  if (button == H2_FRUIT_SLICE_BUTTON_BACK) {
    h2_fruit_slice_game_back_rect(game->width, game->height, out);
    return true;
  }
  if (game->phase == H2_FRUIT_SLICE_PHASE_CARDS) return false;
  const uint32_t base =
      tasks ? H2_FRUIT_SLICE_BUTTON_CLAIM : H2_FRUIT_SLICE_BUTTON_WEAPON;
  if (button < base || button >= base + 3u) return false;
  const float row_gap = fmaxf(4.0f, 6.0f * game->unit);
  const float top = h * (h < 240.0f ? 0.225f : 0.23f);
  const float bottom =
      footer_y - (h >= 280.0f ? fmaxf(24.0f, 32.0f * game->unit) : 4.0f);
  const float row_h = (bottom - top - 2.0f * row_gap) / 3.0f;
  const float y = top + (row_h + row_gap) * (float)(button - base);
  *out = (h2_fruit_slice_rect_t){w * 0.70f, y, w * 0.95f, y + row_h};
  return true;
}

static uint32_t button_at(const h2_fruit_slice_game_t* game, float x, float y) {
  for (uint32_t id = 1u; id < H2_FRUIT_SLICE_BUTTON_WEAPON + 3u; ++id) {
    h2_fruit_slice_rect_t r;
    if (h2_fruit_slice_game_button_rect(game, id, &r) && x >= r.x0 &&
        x <= r.x1 && y >= r.y0 && y <= r.y1)
      return id;
  }
  return 0u;
}

static bool low_sound(unsigned sound) {
  return sound == H2_FRUIT_SLICE_SOUND_SWING ||
         sound == H2_FRUIT_SLICE_SOUND_SLICE ||
         sound == H2_FRUIT_SLICE_SOUND_COIN;
}

bool h2_fruit_slice_game_sound(h2_fruit_slice_game_t* game,
                               h2_fruit_slice_sound_t sound) {
  if (game == NULL || (unsigned)sound >= H2_FRUIT_SLICE_SOUND_COUNT)
    return false;
  if (low_sound((unsigned)sound)) {
    for (unsigned i = 0; i < game->sound_count; ++i)
      if (game->sounds[i] == (unsigned)sound) return true;
  }
  if (game->sound_count == H2_FRUIT_SLICE_SOUND_QUEUE) {
    if (low_sound((unsigned)sound)) return false;
    unsigned i = 0;
    while (i < game->sound_count && !low_sound(game->sounds[i])) ++i;
    if (i == game->sound_count) return false;
    memmove(game->sounds + i, game->sounds + i + 1u,
            game->sound_count - i - 1u);
    --game->sound_count;
  }
  game->sounds[game->sound_count++] = (uint8_t)sound;
  return true;
}

bool h2_fruit_slice_game_take_sound(h2_fruit_slice_game_t* game,
                                    h2_fruit_slice_sound_t* sound) {
  if (game == NULL || sound == NULL || game->sound_count == 0u) return false;
  *sound = (h2_fruit_slice_sound_t)game->sounds[0];
  --game->sound_count;
  memmove(game->sounds, game->sounds + 1u, game->sound_count);
  return true;
}

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
  h2_fruit_slice_rect_t start;
  (void)h2_fruit_slice_game_button_rect(game, H2_FRUIT_SLICE_BUTTON_START,
                                        &start);
  const float gap = fmaxf(6.0f, 8.0f * game->unit);
  fruit->body.x = (float)game->width * 0.5f;
  if (game->height < 220 || game->phase == H2_FRUIT_SLICE_PHASE_GAME_OVER) {
    fruit->body.x = fmaxf((float)game->width * 0.14f,
                          fmaxf(8.0f, 12.0f * game->unit) + fruit->radius);
    y = fminf((start.y0 + start.y1) * 0.5f, start.y1 - fruit->radius);
    if (game->height < 220) y = start.y1 - fruit->radius + gap;
  } else {
    y = fminf(y, start.y0 - fruit->radius - gap);
  }
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
  add_menu_fruit(game, H2_FRUIT_SLICE_WATERMELON, (float)game->height * 0.53f);
}

static void enter_playing(h2_fruit_slice_game_t* game) {
  clear_world(game);
  game->phase = H2_FRUIT_SLICE_PHASE_PLAYING;
  game->phase_started_ms = game->time_ms;
  game->score = 0;
  game->lives = H2_FRUIT_SLICE_LIVES;
  game->waves = 0;
  game->new_best = false;
  game->result_sounded = false;
  game->next_wave_ms = game->time_ms + 700u;
  h2_fruit_slice_cards_run_start(&game->cards);
}

static void enter_game_over(h2_fruit_slice_game_t* game) {
  h2_fruit_slice_cards_run_end(&game->cards);
  game->phase = H2_FRUIT_SLICE_PHASE_GAME_OVER;
  game->phase_started_ms = game->time_ms;
  game->pending_spawns = 0;
  game->stroke_slices = 0;
  (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_FAIL);
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
  game->progress.owned = 1u;
  game->progress.score_task = best >= k_tasks[2].target ? 1u : 0u;
  h2_fruit_slice_cards_init(&game->cards, game->width, game->height, seed);
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
    if (game->phase == H2_FRUIT_SLICE_PHASE_PLAYING) {
      h2_fruit_slice_cards_on_bomb_hit(&game->cards, fruit.body.x,
                                       fruit.body.y);
      enter_game_over(game);
    }
    (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_BOMB);
    return;
  }

  if (fruit.menu) enter_playing(game);
  (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_SLICE);
  add_half(game, &fruit, nx, ny, ux, uy);
  add_half(game, &fruit, -nx, -ny, ux, uy);
  const uint16_t juice = h2_fruit_slice_game_juice(kind);
  burst(game, fruit.body.x, fruit.body.y, ux, uy, 12, juice, 240.0f, 5.0f);
  add_splat(game, fruit.body.x, fruit.body.y, fruit.radius * 1.15f, juice);

  if (fruit.menu || game->phase != H2_FRUIT_SLICE_PHASE_PLAYING) return;
  ++game->score;
  ++game->sliced_total;
  if (game->progress.fruit_credit < H2_FRUIT_SLICE_PROGRESS_LIMIT)
    ++game->progress.fruit_credit;
  progress_changed(game);
  score_task_update(game);
  h2_fruit_slice_cards_on_slice(&game->cards, fruit.kind, game->score,
                                game->progress.equipped, fruit.body.x,
                                fruit.body.y);
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
  if (game->progress.combo_credit < H2_FRUIT_SLICE_PROGRESS_LIMIT)
    ++game->progress.combo_credit;
  progress_changed(game);
  score_task_update(game);
  h2_fruit_slice_cards_on_combo(&game->cards, slices, game->score,
                                game->progress.equipped, game->stroke_last_x,
                                game->stroke_last_y);
  (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_COMBO);
  (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_CHEER);
  char text[H2_FRUIT_SLICE_POPUP_TEXT_MAX];
  (void)snprintf(text, sizeof(text),
                 h2_fruit_slice_translate(game->language, "COMBO +%u"),
                 (unsigned)slices);
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
  if (length2 > 400.0f * game->unit * game->unit)
    (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_SWING);
  const float length = sqrtf(length2);
  const float ux = dx / length, uy = dy / length;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i) {
    const h2_fruit_slice_fruit_t* fruit = &game->fruits[i];
    if (!fruit->active) continue;
    const float reach =
        fruit->radius * 0.95f +
        (fruit->menu ? 0.0f
                     : h2_fruit_slice_weapon(game->progress.equipped)->reach *
                           game->unit);
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

static void activate_button(h2_fruit_slice_game_t* game, uint32_t button) {
  if (button == H2_FRUIT_SLICE_BUTTON_LANGUAGE) {
    game->language = game->language == H2_FRUIT_SLICE_LANGUAGE_EN
                         ? H2_FRUIT_SLICE_LANGUAGE_ZH_CN
                         : H2_FRUIT_SLICE_LANGUAGE_EN;
    return;
  }
  if (button == H2_FRUIT_SLICE_BUTTON_START) {
    if (game->phase == H2_FRUIT_SLICE_PHASE_TITLE ||
        (game->phase == H2_FRUIT_SLICE_PHASE_GAME_OVER &&
         game->time_ms - game->phase_started_ms >=
             H2_FRUIT_SLICE_RESTART_DELAY_MS))
      enter_playing(game);
    return;
  }
  if (button == H2_FRUIT_SLICE_BUTTON_SFX ||
      button == H2_FRUIT_SLICE_BUTTON_MUSIC) {
    game->audio_mute ^= button == H2_FRUIT_SLICE_BUTTON_SFX
                            ? H2_FRUIT_SLICE_MUTE_SFX
                            : H2_FRUIT_SLICE_MUTE_MUSIC;
    (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_EQUIP);
    return;
  }
  if (button == H2_FRUIT_SLICE_BUTTON_TASKS ||
      button == H2_FRUIT_SLICE_BUTTON_SHOP) {
    game->return_phase = game->phase;
    game->phase = button == H2_FRUIT_SLICE_BUTTON_TASKS
                      ? H2_FRUIT_SLICE_PHASE_TASKS
                      : H2_FRUIT_SLICE_PHASE_SHOP;
    game->trail_count = 0u;
    return;
  }
  if (button == H2_FRUIT_SLICE_BUTTON_CARDS) {
    game->return_phase = game->phase;
    game->phase = H2_FRUIT_SLICE_PHASE_CARDS;
    game->trail_count = 0u;
    h2_fruit_slice_cards_open_album(&game->cards);
    return;
  }
  if (button == H2_FRUIT_SLICE_BUTTON_BACK) {
    if (game->phase == H2_FRUIT_SLICE_PHASE_CARDS)
      h2_fruit_slice_cards_close_album(&game->cards);
    game->phase = game->return_phase;
    return;
  }
  h2_fruit_slice_progress_t* p = &game->progress;
  if (button >= H2_FRUIT_SLICE_BUTTON_CLAIM &&
      button < H2_FRUIT_SLICE_BUTTON_CLAIM + H2_FRUIT_SLICE_TASKS) {
    const unsigned id = button - H2_FRUIT_SLICE_BUTTON_CLAIM;
    const h2_fruit_slice_task_t* task = h2_fruit_slice_task(id);
    if (h2_fruit_slice_task_progress(p, id) < task->target ||
        (id == 2u && p->score_task == 2u) ||
        p->coins > H2_FRUIT_SLICE_PROGRESS_LIMIT - task->reward)
      return;
    p->coins += task->reward;
    if (id == 0u) p->fruit_credit -= task->target;
    if (id == 1u) p->combo_credit -= task->target;
    if (id == 2u) p->score_task = 2u;
    game->progress_urgent = true;
    progress_changed(game);
    (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_TASK);
    (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_COIN);
  } else if (button >= H2_FRUIT_SLICE_BUTTON_WEAPON &&
             button < H2_FRUIT_SLICE_BUTTON_WEAPON + H2_FRUIT_SLICE_WEAPONS) {
    const unsigned id = button - H2_FRUIT_SLICE_BUTTON_WEAPON;
    const h2_fruit_slice_weapon_t* weapon = h2_fruit_slice_weapon(id);
    if ((p->owned & (1u << id)) == 0u) {
      if (p->coins < weapon->price) return;
      p->coins -= weapon->price;
      p->owned |= 1u << id;
      (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_PURCHASE);
    }
    if (p->equipped == id) return;
    p->equipped = id;
    game->progress_urgent = true;
    progress_changed(game);
    (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_EQUIP);
  }
}

void h2_fruit_slice_game_pointer(h2_fruit_slice_game_t* game,
                                 h2_fruit_slice_pointer_t kind, float x,
                                 float y) {
  if (h2_fruit_slice_cards_reveal_active(&game->cards)) {
    (void)h2_fruit_slice_cards_reveal_pointer(&game->cards, kind);
    game->pointer_down = game->has_last = false;
    game->pressed_button = 0u;
    return;
  }
  /* Consume the final stroke while waiting for the first reveal. */
  if (game->phase == H2_FRUIT_SLICE_PHASE_GAME_OVER &&
      game->cards.reveal_queue != 0u) {
    game->pointer_down = game->has_last = false;
    game->pressed_button = 0u;
    return;
  }
  if (kind == H2_FRUIT_SLICE_POINTER_DOWN) {
    game->pressed_button = button_at(game, x, y);
  }
  if (game->phase == H2_FRUIT_SLICE_PHASE_CARDS && game->pressed_button == 0u) {
    h2_fruit_slice_cards_album_pointer(&game->cards, kind, x, y);
    return;
  }
  if (game->pressed_button != 0u || game->phase == H2_FRUIT_SLICE_PHASE_TASKS ||
      game->phase == H2_FRUIT_SLICE_PHASE_SHOP) {
    /* Only a down/up on the same button activates; a swipe cannot buy or
     * claim multiple rows and cancellation never activates a button. */
    if (kind == H2_FRUIT_SLICE_POINTER_UP ||
        kind == H2_FRUIT_SLICE_POINTER_CANCEL) {
      const uint32_t pressed = game->pressed_button;
      game->pressed_button = 0u;
      game->pointer_down = game->has_last = false;
      if (kind == H2_FRUIT_SLICE_POINTER_UP && pressed != 0u &&
          pressed == button_at(game, x, y))
        activate_button(game, pressed);
    }
    return;
  }
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
  (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_MISS);
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
  const bool allow_reveal =
      game->phase == H2_FRUIT_SLICE_PHASE_GAME_OVER &&
      game->time_ms - game->phase_started_ms >= H2_FRUIT_SLICE_REVEAL_DELAY_MS;
  if (allow_reveal && game->cards.reveal_queue != 0u && !game->result_sounded) {
    game->result_sounded = true;
    (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_RESULT);
  }
  h2_fruit_slice_cards_step(&game->cards, ms, allow_reveal);
  h2_fruit_slice_card_cue_t cue;
  while ((cue = h2_fruit_slice_cards_take_cue(&game->cards)) !=
         H2_FRUIT_SLICE_CARD_CUE_NONE)
    (void)h2_fruit_slice_game_sound(game, cue == H2_FRUIT_SLICE_CARD_CUE_RARE
                                              ? H2_FRUIT_SLICE_SOUND_RARE
                                              : H2_FRUIT_SLICE_SOUND_CARD);
  if (game->phase == H2_FRUIT_SLICE_PHASE_CARDS || game->cards.reveal_active)
    return;
  /* Menus pause the scene, including restart delays and trail ages. */
  if (game->phase == H2_FRUIT_SLICE_PHASE_TASKS ||
      game->phase == H2_FRUIT_SLICE_PHASE_SHOP)
    return;
  game->time_ms += ms;
  if (game->phase == H2_FRUIT_SLICE_PHASE_GAME_OVER && !game->result_sounded &&
      game->time_ms - game->phase_started_ms >=
          H2_FRUIT_SLICE_RESTART_DELAY_MS) {
    game->result_sounded = true;
    (void)h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_RESULT);
  }
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
      if (game->phase == H2_FRUIT_SLICE_PHASE_PLAYING) {
        if (fruit->kind == H2_FRUIT_SLICE_BOMB)
          h2_fruit_slice_cards_on_bomb_passed(&game->cards, fruit->body.x, h);
        else
          record_miss(game, fruit->body.x);
      }
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
    add_menu_fruit(game, H2_FRUIT_SLICE_ORANGE, h * 0.57f);

  update_spawns(game);
}

void h2_fruit_slice_game_step(h2_fruit_slice_game_t* game, uint32_t dt_ms) {
  while (dt_ms > 0) {
    const uint32_t ms = dt_ms < STEP_MAX_MS ? dt_ms : STEP_MAX_MS;
    step_once(game, ms);
    dt_ms -= ms;
  }
}
