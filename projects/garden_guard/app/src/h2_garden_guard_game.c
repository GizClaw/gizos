#include "h2_garden_guard_game.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define STEP_MAX_MS 20u
#define CARD_DENIED_MS 450u
/* Far lawn width over near lawn width. */
#define FAR_RATIO 0.7f
/* Deepest lawn row, in near cell widths per lane. */
#define MAX_DEPTH_PER_LANE 1.2f
/* Characters stand slightly in front of their lane's centre line. */
#define FOOT_IN_LANE 0.58f
#define SKY_SUN_FIRST_MS 5000u
#define SUNFLOWER_FIRST_MS 6000u
#define SUNFLOWER_PERIOD_MS 12000u
#define SHOOTER_PERIOD_MS 1400
#define PEA_DAMAGE 20
#define PEA_SPEED 5.5f /* Cells per second. */
#define CART_SPEED 6.0f
#define PEST_ENTRY_X ((float)H2_GARDEN_GUARD_COLS + 0.45f)
#define WAVE_AFTER_LAST_SPAWN_MS 16000u
#define WAVE_AFTER_CLEAR_MS 4000u

typedef struct plant_spec {
  uint32_t cost, cooldown_ms;
  float hp;
} plant_spec_t;

static const plant_spec_t k_plants[H2_GARDEN_GUARD_PLANT_KIND_COUNT] = {
    [H2_GARDEN_GUARD_PLANT_NONE] = {0u, 0u, 0.0f},
    [H2_GARDEN_GUARD_PLANT_SUNFLOWER] = {50u, 5000u, 300.0f},
    [H2_GARDEN_GUARD_PLANT_SHOOTER] = {100u, 5000u, 300.0f},
    [H2_GARDEN_GUARD_PLANT_PUMPKIN] = {50u, 18000u, 2400.0f},
    [H2_GARDEN_GUARD_PLANT_FROST] = {125u, 7000u, 320.0f},
    [H2_GARDEN_GUARD_PLANT_BRAMBLE] = {75u, 9000u, 1050.0f},
};

typedef struct pest_spec {
  int32_t hp, armor;
  float speed;    /* Cells per second. */
  float bite_dps; /* Plant health per second while chewing. */
  uint32_t score;
} pest_spec_t;

static const pest_spec_t k_pests[H2_GARDEN_GUARD_PEST_KIND_COUNT] = {
    [H2_GARDEN_GUARD_PEST_SHAMBLER] = {190, 0, 0.21f, 55.0f, 10u},
    [H2_GARDEN_GUARD_PEST_SHELLBACK] = {190, 330, 0.19f, 55.0f, 25u},
    [H2_GARDEN_GUARD_PEST_RUNNER] = {110, 0, 0.42f, 40.0f, 15u},
};

typedef struct wave_spec {
  uint8_t count[H2_GARDEN_GUARD_PEST_KIND_COUNT];
  uint16_t gap_min_ms, gap_max_ms;
} wave_spec_t;

static const wave_spec_t k_waves[H2_GARDEN_GUARD_WAVES] = {
    {{3, 0, 0}, 5500u, 7500u}, {{4, 1, 0}, 4000u, 6000u},
    {{4, 2, 1}, 3000u, 4500u}, {{5, 3, 2}, 2200u, 3600u},
    {{7, 4, 3}, 900u, 2000u},
};

static uint32_t random_next(h2_garden_guard_game_t* game) {
  uint32_t x = game->rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  game->rng = x;
  return x;
}

/* Uniform in [0, 1). */
static float random_unit(h2_garden_guard_game_t* game) {
  return (float)(random_next(game) >> 8) * (1.0f / 16777216.0f);
}

static float random_range(h2_garden_guard_game_t* game, float low, float high) {
  return low + (high - low) * random_unit(game);
}

static uint32_t random_below(h2_garden_guard_game_t* game, uint32_t limit) {
  return limit != 0u ? random_next(game) % limit : 0u;
}

static h2_garden_guard_plant_kind_t card_plant(int32_t card);

uint32_t h2_garden_guard_card_cost(h2_garden_guard_card_t card) {
  return k_plants[card_plant(card)].cost;
}

uint32_t h2_garden_guard_card_cooldown(h2_garden_guard_card_t card) {
  return k_plants[card_plant(card)].cooldown_ms;
}

uint8_t h2_garden_guard_card_unlock(int32_t card) {
  if (card == H2_GARDEN_GUARD_CARD_FROST) return H2_GARDEN_GUARD_FROST_BIT;
  if (card == H2_GARDEN_GUARD_CARD_BRAMBLE) return H2_GARDEN_GUARD_BRAMBLE_BIT;
  return 0u;
}

uint32_t h2_garden_guard_offer_price(unsigned offer) {
  return offer == 0u ? 6u : 12u;
}

float h2_garden_guard_plant_max_hp(h2_garden_guard_plant_kind_t kind) {
  if ((unsigned)kind >= H2_GARDEN_GUARD_PLANT_KIND_COUNT) return 0.0f;
  return k_plants[kind].hp;
}

static h2_garden_guard_plant_kind_t card_plant(int32_t card) {
  switch (card) {
    case H2_GARDEN_GUARD_CARD_SUNFLOWER:
      return H2_GARDEN_GUARD_PLANT_SUNFLOWER;
    case H2_GARDEN_GUARD_CARD_SHOOTER:
      return H2_GARDEN_GUARD_PLANT_SHOOTER;
    case H2_GARDEN_GUARD_CARD_PUMPKIN:
      return H2_GARDEN_GUARD_PLANT_PUMPKIN;
    case H2_GARDEN_GUARD_CARD_FROST:
      return H2_GARDEN_GUARD_PLANT_FROST;
    case H2_GARDEN_GUARD_CARD_BRAMBLE:
      return H2_GARDEN_GUARD_PLANT_BRAMBLE;
    default:
      return H2_GARDEN_GUARD_PLANT_NONE;
  }
}

uint32_t h2_garden_guard_pests_total(void) {
  uint32_t total = 0;
  for (unsigned w = 0; w < H2_GARDEN_GUARD_WAVES; ++w)
    for (unsigned k = 0; k < H2_GARDEN_GUARD_PEST_KIND_COUNT; ++k)
      total += k_waves[w].count[k];
  return total;
}

static uint32_t wave_left_total(const h2_garden_guard_game_t* game) {
  uint32_t total = 0;
  for (unsigned k = 0; k < H2_GARDEN_GUARD_PEST_KIND_COUNT; ++k)
    total += game->wave_left[k];
  return total;
}

static uint32_t active_pests(const h2_garden_guard_game_t* game) {
  uint32_t count = 0;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PESTS; ++i)
    if (game->pests[i].active) ++count;
  return count;
}

uint32_t h2_garden_guard_pests_remaining(const h2_garden_guard_game_t* game) {
  uint32_t total = wave_left_total(game) + active_pests(game);
  for (uint32_t w = game->wave; w < H2_GARDEN_GUARD_WAVES; ++w)
    for (unsigned k = 0; k < H2_GARDEN_GUARD_PEST_KIND_COUNT; ++k)
      total += k_waves[w].count[k];
  return total;
}

static void compute_layout(h2_garden_guard_layout_t* l, int32_t width,
                           int32_t height) {
  memset(l, 0, sizeof(*l));
  l->width = width;
  l->height = height;
  const float w = (float)width, h = (float)height;
  l->unit = fminf(w, h) / 480.0f;
  l->portrait = height > width;
  float bar = floorf(fminf(h * 0.165f, w * 0.135f));
  if (bar < 20.0f) bar = 20.0f;
  l->bar_h = bar;

  /* A patio on the house side and an entry path on the other frame the
   * lawn; the front wall below its near edge shows that it is raised. */
  const float patio = floorf(w * 0.075f), path = floorf(w * 0.045f);
  l->near_w = w - patio - path;
  l->mid_x = patio + l->near_w * 0.5f;
  l->far_ratio = FAR_RATIO;
  l->sprite = l->near_w / (float)H2_GARDEN_GUARD_COLS;
  const float facade = floorf(fmaxf(h * 0.066f, 8.0f));
  l->near_y = h - facade;
  /* Keep far-row figures clear of the toolbar, and cap the depth so the
   * camera does not look straight down on tall displays. */
  const float courtyard = fmaxf(l->sprite * FAR_RATIO * 1.22f, h * 0.06f);
  const float depth =
      fminf(l->near_y - bar - courtyard,
            l->sprite * (float)H2_GARDEN_GUARD_LANES * MAX_DEPTH_PER_LANE);
  l->far_y = l->near_y - depth;
  l->field_x = patio;
  l->field_y = l->far_y;
  l->field_w = l->near_w;
  l->field_h = depth;

  float pad = floorf(bar * 0.08f);
  if (pad < 1.0f) pad = 1.0f;
  l->card_y = pad;
  l->card_h = bar - 2.0f * pad;
  l->card_w = floorf(fminf(l->card_h * 0.82f, w * 0.103f));
  l->sun_x = pad;
  l->sun_w = floorf(l->card_h * 1.25f);
  float x = l->sun_x + l->sun_w + 2.0f * pad;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_CARD_COUNT; ++i) {
    l->card_x[i] = x;
    x += l->card_w + pad;
  }
}

float h2_garden_guard_depth_scale(const h2_garden_guard_layout_t* layout,
                                  float f) {
  return 1.0f / (1.0f + f * (1.0f / layout->far_ratio - 1.0f));
}

/* Screen rows are linear in the perspective factor t. */
static float row_of_scale(const h2_garden_guard_layout_t* l, float t) {
  return l->far_y +
         (l->near_y - l->far_y) * (t - l->far_ratio) / (1.0f - l->far_ratio);
}

void h2_garden_guard_ground_point(const h2_garden_guard_layout_t* layout,
                                  float u, float f, float* out_x,
                                  float* out_y) {
  const float t = h2_garden_guard_depth_scale(layout, f);
  *out_x = layout->mid_x + (u - 0.5f) * layout->near_w * t;
  *out_y = row_of_scale(layout, t);
}

bool h2_garden_guard_ground_at(const h2_garden_guard_layout_t* layout, float x,
                               float y, float* out_u, float* out_f) {
  const float t = layout->far_ratio + (y - layout->far_y) /
                                          (layout->near_y - layout->far_y) *
                                          (1.0f - layout->far_ratio);
  if (!(t > 0.05f)) return false;
  *out_f = (1.0f / t - 1.0f) / (1.0f / layout->far_ratio - 1.0f);
  *out_u = 0.5f + (x - layout->mid_x) / (layout->near_w * t);
  return true;
}

float h2_garden_guard_lane_depth(uint32_t lane) {
  return 1.0f - ((float)lane + FOOT_IN_LANE) / (float)H2_GARDEN_GUARD_LANES;
}

float h2_garden_guard_lane_size(const h2_garden_guard_layout_t* layout,
                                uint32_t lane) {
  return layout->sprite *
         h2_garden_guard_depth_scale(layout, h2_garden_guard_lane_depth(lane));
}

void h2_garden_guard_field_point(const h2_garden_guard_layout_t* layout,
                                 uint32_t lane, float along, float* out_x,
                                 float* out_y) {
  h2_garden_guard_ground_point(layout, along / (float)H2_GARDEN_GUARD_COLS,
                               h2_garden_guard_lane_depth(lane), out_x, out_y);
}

bool h2_garden_guard_cell_at(const h2_garden_guard_layout_t* layout, float x,
                             float y, uint32_t* out_lane, uint32_t* out_col) {
  float u, f;
  if (!h2_garden_guard_ground_at(layout, x, y, &u, &f)) return false;
  const float lane = (1.0f - f) * (float)H2_GARDEN_GUARD_LANES;
  const float along = u * (float)H2_GARDEN_GUARD_COLS;
  if (!(lane >= 0.0f && lane < (float)H2_GARDEN_GUARD_LANES && along >= 0.0f &&
        along < (float)H2_GARDEN_GUARD_COLS))
    return false;
  *out_lane = (uint32_t)lane;
  *out_col = (uint32_t)along;
  return true;
}

float h2_garden_guard_plant_height(h2_garden_guard_plant_kind_t kind) {
  switch (kind) {
    case H2_GARDEN_GUARD_PLANT_SUNFLOWER:
      return 1.17f;
    case H2_GARDEN_GUARD_PLANT_FROST:
    case H2_GARDEN_GUARD_PLANT_SHOOTER:
      return 1.06f;
    case H2_GARDEN_GUARD_PLANT_BRAMBLE:
    case H2_GARDEN_GUARD_PLANT_PUMPKIN:
      return 0.72f;
    default:
      return 0.0f;
  }
}

h2_garden_guard_plant_pose_t h2_garden_guard_plant_pose(
    const h2_garden_guard_game_t* game, uint32_t lane, uint32_t col) {
  const h2_garden_guard_plant_t* plant = &game->plants[lane][col];
  const uint64_t idle = game->time_ms / H2_GARDEN_GUARD_PLANT_IDLE_MS;
  const uint64_t t =
      idle * H2_GARDEN_GUARD_PLANT_IDLE_MS + lane * 311u + col * 197u;
  const float remaining = (float)plant->grow_ms / H2_GARDEN_GUARD_PLANT_GROW_MS;
  h2_garden_guard_plant_pose_t pose = {
      .growth = 1.0f - 0.56f * remaining * remaining,
      .impact = (float)plant->impact_ms / H2_GARDEN_GUARD_BITE_IMPACT_MS};
  switch (plant->kind) {
    case H2_GARDEN_GUARD_PLANT_SUNFLOWER:
      pose.sway = 0.075f * sinf((float)(t % 2400u) *
                                (2.0f * H2_GARDEN_GUARD_PI / 2400.0f));
      pose.blink = t % 3360u < H2_GARDEN_GUARD_PLANT_IDLE_MS;
      pose.action = (float)plant->action_ms / H2_GARDEN_GUARD_BLOOM_POSE_MS;
      pose.lift = 0.09f * pose.action;
      break;
    case H2_GARDEN_GUARD_PLANT_FROST:
    case H2_GARDEN_GUARD_PLANT_SHOOTER:
      pose.sway = 0.033f * sinf((float)(t % 1680u) *
                                (2.0f * H2_GARDEN_GUARD_PI / 1680.0f));
      pose.blink = t % 2640u < H2_GARDEN_GUARD_PLANT_IDLE_MS;
      pose.action = (float)plant->action_ms / H2_GARDEN_GUARD_SHOT_POSE_MS;
      break;
    case H2_GARDEN_GUARD_PLANT_BRAMBLE:
    case H2_GARDEN_GUARD_PLANT_PUMPKIN:
      pose.lift = 0.022f * sinf((float)(t % 3960u) *
                                (2.0f * H2_GARDEN_GUARD_PI / 3960.0f));
      pose.blink = t % 4680u < H2_GARDEN_GUARD_PLANT_IDLE_MS;
      break;
    default:
      break;
  }
  return pose;
}

bool h2_garden_guard_plant_at(const h2_garden_guard_game_t* game, float x,
                              float y, uint32_t* out_lane, uint32_t* out_col) {
  /* Nearer figures are drawn over farther ones, so they win the hit. */
  for (uint32_t i = 0; i < H2_GARDEN_GUARD_LANES; ++i) {
    const uint32_t lane = H2_GARDEN_GUARD_LANES - 1u - i;
    const float size = h2_garden_guard_lane_size(&game->layout, lane);
    for (uint32_t col = 0; col < H2_GARDEN_GUARD_COLS; ++col) {
      const uint8_t kind = game->plants[lane][col].kind;
      if (kind == H2_GARDEN_GUARD_PLANT_NONE) continue;
      float gx, gy;
      h2_garden_guard_field_point(&game->layout, lane, (float)col + 0.5f, &gx,
                                  &gy);
      const h2_garden_guard_plant_pose_t pose =
          h2_garden_guard_plant_pose(game, lane, col);
      const float s = size * pose.growth;
      // Follow the raised head, including growth and recoil, with touch slop.
      const float raised = fmaxf(0.0f, (gy - y) / s);
      const float shift =
          kind == H2_GARDEN_GUARD_PLANT_PUMPKIN ||
                  kind == H2_GARDEN_GUARD_PLANT_BRAMBLE
              ? -0.075f * pose.impact
              : (pose.sway - 0.19f * pose.action * pose.action *
                                 (kind == H2_GARDEN_GUARD_PLANT_SHOOTER ||
                                  kind == H2_GARDEN_GUARD_PLANT_FROST)) *
                    fminf(1.0f, raised / 0.62f);
      const float top =
          gy -
          (h2_garden_guard_plant_height((h2_garden_guard_plant_kind_t)kind) +
           fmaxf(0.0f, pose.lift) + 0.06f) *
              s;
      if (fabsf(x - gx - shift * s) <= 0.46f * s && y >= top &&
          y <= gy + 0.12f * s) {
        *out_lane = lane;
        *out_col = col;
        return true;
      }
    }
  }
  return false;
}

int32_t h2_garden_guard_card_at(const h2_garden_guard_layout_t* layout, float x,
                                float y) {
  /* Cards fill the toolbar height; half the gap counts toward each card. */
  if (!(y >= 0.0f && y < layout->bar_h)) return H2_GARDEN_GUARD_CARD_NONE;
  const float slack = layout->card_y * 0.5f + 0.5f;
  for (int32_t i = 0; i < H2_GARDEN_GUARD_CARD_COUNT; ++i)
    if (x >= layout->card_x[i] - slack &&
        x < layout->card_x[i] + layout->card_w + slack)
      return i;
  return H2_GARDEN_GUARD_CARD_NONE;
}

void h2_garden_guard_sun_counter_point(const h2_garden_guard_layout_t* layout,
                                       float* out_x, float* out_y) {
  *out_x = layout->sun_x + layout->sun_w * 0.5f;
  *out_y = layout->card_y + layout->card_h * 0.34f;
}

float h2_garden_guard_sun_radius(const h2_garden_guard_layout_t* layout) {
  return fmaxf(layout->sprite * 0.3f, 9.0f * layout->unit);
}

static void set_banner(h2_garden_guard_game_t* game, const char* text) {
  (void)snprintf(game->banner, sizeof(game->banner), "%s", text);
  game->banner_ms = H2_GARDEN_GUARD_BANNER_MS;
}

static void add_particles(h2_garden_guard_game_t* game, float x, float y,
                          unsigned count, uint16_t color, float speed,
                          float size) {
  for (unsigned n = 0; n < count; ++n) {
    h2_garden_guard_particle_t* slot = NULL;
    for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PARTICLES; ++i)
      if (!game->particles[i].active) {
        slot = &game->particles[i];
        break;
      }
    if (slot == NULL) return;
    const float angle = random_range(game, 0.0f, 2.0f * H2_GARDEN_GUARD_PI);
    const float v = speed * random_range(game, 0.4f, 1.0f);
    *slot = (h2_garden_guard_particle_t){
        .active = true,
        .x = x,
        .y = y,
        .vx = cosf(angle) * v,
        .vy = sinf(angle) * v,
        .size = size * random_range(game, 0.6f, 1.0f),
        .color = color,
        .life_ms = (uint16_t)random_range(game, 260.0f, 480.0f)};
  }
}

static void clear_world(h2_garden_guard_game_t* game) {
  memset(game->pests, 0, sizeof(game->pests));
  memset(game->peas, 0, sizeof(game->peas));
  memset(game->suns, 0, sizeof(game->suns));
  game->selected = H2_GARDEN_GUARD_CARD_NONE;
  game->drag_from_card = false;
  game->hover_lane = -1;
  game->hover_col = -1;
  game->banner_ms = 0;
  game->sighting_ms = 0;
}

static void enter_title(h2_garden_guard_game_t* game) {
  clear_world(game);
  memset(game->particles, 0, sizeof(game->particles));
  game->phase = H2_GARDEN_GUARD_PHASE_TITLE;
  game->phase_started_ms = game->time_ms;
}

static void enter_playing(h2_garden_guard_game_t* game) {
  clear_world(game);
  memset(game->particles, 0, sizeof(game->particles));
  memset(game->plants, 0, sizeof(game->plants));
  memset(game->carts, 0, sizeof(game->carts));
  memset(game->card_cooldown_ms, 0, sizeof(game->card_cooldown_ms));
  memset(game->card_denied_ms, 0, sizeof(game->card_denied_ms));
  memset(game->wave_left, 0, sizeof(game->wave_left));
  ++game->plant_revision;
  game->phase = H2_GARDEN_GUARD_PHASE_PLAYING;
  game->phase_started_ms = game->time_ms;
  game->sun = H2_GARDEN_GUARD_START_SUN;
  game->score = 0;
  game->defense_score = 0;
  game->earned_coins = 0;
  game->kills = 0;
  game->new_best = false;
  game->wave = 0;
  game->next_wave_ms = game->time_ms + H2_GARDEN_GUARD_FIRST_WAVE_MS;
  game->next_spawn_ms = UINT64_MAX;
  game->next_sky_sun_ms = game->time_ms + SKY_SUN_FIRST_MS;
  game->last_lane = UINT32_MAX;
  set_banner(game, "GET READY");
}

static uint32_t add_capped(uint32_t a, uint32_t b) {
  return UINT32_MAX - a < b ? UINT32_MAX : a + b;
}

static void finish(h2_garden_guard_game_t* game, bool won) {
  if (game->phase != H2_GARDEN_GUARD_PHASE_PLAYING) return;
  clear_world(game);
  if (won) game->score += H2_GARDEN_GUARD_WIN_BONUS;
  /* Free carts still score, but only damage dealt by plants earns coins. */
  game->earned_coins = 3u * ((game->defense_score + 9u) / 10u);
  if (won && game->defense_score != 0u) game->earned_coins += 12u;
  game->profile.coins = add_capped(game->profile.coins, game->earned_coins);
  game->profile.total_score =
      add_capped(game->profile.total_score, game->score);
  game->profile_dirty = true;
  game->phase = won ? H2_GARDEN_GUARD_PHASE_WON : H2_GARDEN_GUARD_PHASE_LOST;
  game->phase_started_ms = game->time_ms;
  if (game->score > game->best) {
    game->best = game->score;
    game->profile.best = game->best;
    game->best_changed = true;
    game->new_best = true;
  }
}

void h2_garden_guard_game_init(h2_garden_guard_game_t* game, int32_t width,
                               int32_t height, uint32_t seed, uint32_t best) {
  memset(game, 0, sizeof(*game));
  compute_layout(&game->layout, width, height);
  game->rng = seed != 0u ? seed : 0x6A09E667u;
  game->best = best;
  game->profile.best = best;
  game->sun = H2_GARDEN_GUARD_START_SUN;
  game->selected = H2_GARDEN_GUARD_CARD_NONE;
  enter_title(game);
}

bool h2_garden_guard_game_take_best(h2_garden_guard_game_t* game,
                                    uint32_t* out_best) {
  if (!game->best_changed) return false;
  game->best_changed = false;
  if (out_best != NULL) *out_best = game->best;
  return true;
}

static bool card_usable(const h2_garden_guard_game_t* game, int32_t card) {
  if (card < 0 || card >= H2_GARDEN_GUARD_CARD_COUNT) return false;
  const uint8_t unlock = h2_garden_guard_card_unlock(card);
  return (game->profile.owned & unlock) == unlock &&
         game->card_cooldown_ms[card] == 0u &&
         game->sun >= h2_garden_guard_card_cost((h2_garden_guard_card_t)card);
}

static void add_sun(h2_garden_guard_game_t* game, float x, float y,
                    float rest_y) {
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_SUNS; ++i) {
    h2_garden_guard_sun_t* sun = &game->suns[i];
    if (sun->active) continue;
    *sun = (h2_garden_guard_sun_t){
        .active = true, .x = x, .y = y, .rest_y = rest_y};
    return;
  }
  /* Every slot is busy: credit the sun rather than lose it. */
  game->sun += H2_GARDEN_GUARD_SUN_VALUE;
  if (game->sun > H2_GARDEN_GUARD_SUN_LIMIT)
    game->sun = H2_GARDEN_GUARD_SUN_LIMIT;
}

/* Collect every uncollected sun under the point; true when one was taken. */
static bool collect_suns_at(h2_garden_guard_game_t* game, float x, float y) {
  const float reach = h2_garden_guard_sun_radius(&game->layout) * 1.6f;
  float counter_x, counter_y;
  h2_garden_guard_sun_counter_point(&game->layout, &counter_x, &counter_y);
  bool taken = false;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_SUNS; ++i) {
    h2_garden_guard_sun_t* sun = &game->suns[i];
    if (!sun->active || sun->collecting) continue;
    /* A sun still behind the toolbar must not steal a card tap. */
    if (sun->y < game->layout.bar_h) continue;
    const float dx = sun->x - x, dy = sun->y - y;
    if (dx * dx + dy * dy > reach * reach) continue;
    sun->collecting = true;
    const float tx = counter_x - sun->x, ty = counter_y - sun->y;
    const float distance = sqrtf(tx * tx + ty * ty);
    const float speed = 1100.0f * game->layout.unit;
    sun->vx = distance > 0.0f ? tx / distance * speed : 0.0f;
    sun->vy = distance > 0.0f ? ty / distance * speed : 0.0f;
    game->sun += H2_GARDEN_GUARD_SUN_VALUE;
    if (game->sun > H2_GARDEN_GUARD_SUN_LIMIT)
      game->sun = H2_GARDEN_GUARD_SUN_LIMIT;
    taken = true;
  }
  return taken;
}

static void deny(h2_garden_guard_game_t* game, int32_t card) {
  if (card >= 0 && card < H2_GARDEN_GUARD_CARD_COUNT)
    game->card_denied_ms[card] = CARD_DENIED_MS;
}

static uint8_t plant_stage(const h2_garden_guard_plant_t* plant) {
  const float max =
      h2_garden_guard_plant_max_hp((h2_garden_guard_plant_kind_t)plant->kind);
  if (plant->hp * 3.0f <= max) return 2u;
  if (plant->hp * 3.0f <= max * 2.0f) return 1u;
  return 0u;
}

static void plant_burst(h2_garden_guard_game_t* game, uint32_t lane,
                        uint32_t col, uint16_t color) {
  float x, y;
  h2_garden_guard_field_point(&game->layout, lane, (float)col + 0.5f, &x, &y);
  const float size = h2_garden_guard_lane_size(&game->layout, lane);
  add_particles(game, x, y - 0.3f * size, 8u, color, 110.0f * game->layout.unit,
                3.5f * game->layout.unit);
}

static void remove_plant(h2_garden_guard_game_t* game, uint32_t lane,
                         uint32_t col, uint16_t color) {
  memset(&game->plants[lane][col], 0, sizeof(game->plants[lane][col]));
  ++game->plant_revision;
  plant_burst(game, lane, col, color);
}

/* Use the selected card on a cell. */
static void apply_card(h2_garden_guard_game_t* game, uint32_t lane,
                       uint32_t col) {
  const int32_t card = game->selected;
  h2_garden_guard_plant_t* plant = &game->plants[lane][col];
  if (card == H2_GARDEN_GUARD_CARD_SHOVEL) {
    if (plant->kind != H2_GARDEN_GUARD_PLANT_NONE)
      remove_plant(game, lane, col, H2_GARDEN_GUARD_RGB(122, 86, 50));
    game->selected = H2_GARDEN_GUARD_CARD_NONE;
    return;
  }
  const h2_garden_guard_plant_kind_t kind = card_plant(card);
  if (kind == H2_GARDEN_GUARD_PLANT_NONE) return;
  /* Keep the selection on an occupied cell so another cell can be tapped. */
  if (plant->kind != H2_GARDEN_GUARD_PLANT_NONE) return;
  if (!card_usable(game, card)) {
    deny(game, card);
    game->selected = H2_GARDEN_GUARD_CARD_NONE;
    return;
  }
  game->sun -= h2_garden_guard_card_cost((h2_garden_guard_card_t)card);
  game->card_cooldown_ms[card] =
      h2_garden_guard_card_cooldown((h2_garden_guard_card_t)card);
  *plant = (h2_garden_guard_plant_t){
      .kind = (uint8_t)kind,
      .hp = k_plants[kind].hp,
      .grow_ms = H2_GARDEN_GUARD_PLANT_GROW_MS,
      .timer_ms = kind == H2_GARDEN_GUARD_PLANT_SUNFLOWER
                      ? (int32_t)SUNFLOWER_FIRST_MS
                      : 0};
  ++game->plant_revision;
  game->selected = H2_GARDEN_GUARD_CARD_NONE;
  plant_burst(game, lane, col, H2_GARDEN_GUARD_RGB(150, 112, 70));
}

/* Cell a selected card applies to. Plants stand up out of their cell, so
 * the shovel takes the figure under the point before the ground cell. */
static bool target_cell(const h2_garden_guard_game_t* game, float x, float y,
                        uint32_t* lane, uint32_t* col) {
  if (game->selected == H2_GARDEN_GUARD_CARD_SHOVEL &&
      h2_garden_guard_plant_at(game, x, y, lane, col))
    return true;
  return h2_garden_guard_cell_at(&game->layout, x, y, lane, col);
}

static void clear_hover(h2_garden_guard_game_t* game) {
  game->hover_lane = -1;
  game->hover_col = -1;
}

static float menu_scale(const h2_garden_guard_layout_t* l) {
  return fminf((float)l->width / 480.0f, (float)l->height / 320.0f);
}

h2_garden_guard_rect_t h2_garden_guard_menu_tab(
    const h2_garden_guard_layout_t* l, bool result, unsigned tab) {
  const float s = menu_scale(l);
  const float top = ((float)l->height - 320.0f * s) * 0.5f;
  const float y = top + (result ? 278.0f : 267.0f) * s;
  const float x = (float)l->width * 0.5f + (tab == 0u ? -212.0f : 104.0f) * s;
  return (h2_garden_guard_rect_t){x, y, 108.0f * s, 28.0f * s};
}

h2_garden_guard_rect_t h2_garden_guard_menu_back(
    const h2_garden_guard_layout_t* l) {
  const float s = menu_scale(l);
  return (h2_garden_guard_rect_t){
      ((float)l->width - 440.0f * s) * 0.5f,
      ((float)l->height - 320.0f * s) * 0.5f + 16.0f * s, 80.0f * s, 32.0f * s};
}

h2_garden_guard_rect_t h2_garden_guard_menu_row(
    const h2_garden_guard_layout_t* l, unsigned row) {
  const float s = menu_scale(l);
  const h2_garden_guard_rect_t back = h2_garden_guard_menu_back(l);
  return (h2_garden_guard_rect_t){
      back.x, back.y + (60.0f + (float)row * 70.0f) * s, 440.0f * s, 62.0f * s};
}

static bool contains(h2_garden_guard_rect_t r, float x, float y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void press_menu(h2_garden_guard_game_t* game, float x, float y) {
  const h2_garden_guard_layout_t* l = &game->layout;
  if (contains(h2_garden_guard_menu_back(l), x, y)) {
    game->menu = H2_GARDEN_GUARD_MENU_NONE;
    return;
  }
  if (game->menu != H2_GARDEN_GUARD_MENU_SHOP) return;
  for (unsigned offer = 0; offer < 2u; ++offer) {
    const h2_garden_guard_rect_t row = h2_garden_guard_menu_row(l, offer);
    const h2_garden_guard_rect_t button = {row.x + row.w * 0.74f, row.y,
                                           row.w * 0.26f, row.h};
    if (!contains(button, x, y)) continue;
    const uint8_t bit =
        offer == 0u ? H2_GARDEN_GUARD_FROST_BIT : H2_GARDEN_GUARD_BRAMBLE_BIT;
    const uint32_t price = h2_garden_guard_offer_price(offer);
    if ((game->profile.owned & bit) != 0u || game->profile.coins < price)
      return;
    game->profile.coins -= price;
    game->profile.owned |= bit;
    game->profile_dirty = true;
    return;
  }
}

static void press(h2_garden_guard_game_t* game, float x, float y) {
  if (game->menu != H2_GARDEN_GUARD_MENU_NONE) {
    press_menu(game, x, y);
    return;
  }
  if (game->phase != H2_GARDEN_GUARD_PHASE_PLAYING) {
    const bool result = game->phase != H2_GARDEN_GUARD_PHASE_TITLE;
    for (unsigned tab = 0; tab < 2u; ++tab) {
      if (!contains(h2_garden_guard_menu_tab(&game->layout, result, tab), x, y))
        continue;
      game->menu =
          tab == 0u ? H2_GARDEN_GUARD_MENU_JOURNAL : H2_GARDEN_GUARD_MENU_SHOP;
      return;
    }
  }
  switch (game->phase) {
    case H2_GARDEN_GUARD_PHASE_TITLE:
      enter_playing(game);
      return;
    case H2_GARDEN_GUARD_PHASE_WON:
    case H2_GARDEN_GUARD_PHASE_LOST:
      if (game->time_ms - game->phase_started_ms >=
          H2_GARDEN_GUARD_RESTART_DELAY_MS)
        enter_playing(game);
      return;
    default:
      break;
  }
  if (collect_suns_at(game, x, y)) return;
  const int32_t card = h2_garden_guard_card_at(&game->layout, x, y);
  if (card != H2_GARDEN_GUARD_CARD_NONE) {
    if (game->selected == card) {
      game->selected = H2_GARDEN_GUARD_CARD_NONE;
    } else if (card_usable(game, card)) {
      game->selected = card;
      game->drag_from_card = true;
    } else {
      deny(game, card);
      game->selected = H2_GARDEN_GUARD_CARD_NONE;
    }
    return;
  }
  uint32_t lane, col;
  if (game->selected != H2_GARDEN_GUARD_CARD_NONE &&
      target_cell(game, x, y, &lane, &col)) {
    apply_card(game, lane, col);
    return;
  }
  game->selected = H2_GARDEN_GUARD_CARD_NONE;
}

void h2_garden_guard_game_pointer(h2_garden_guard_game_t* game,
                                  h2_garden_guard_pointer_t kind, float x,
                                  float y) {
  switch (kind) {
    case H2_GARDEN_GUARD_POINTER_DOWN:
      game->pointer_down = true;
      game->drag_from_card = false;
      clear_hover(game);
      press(game, x, y);
      break;
    case H2_GARDEN_GUARD_POINTER_MOVE: {
      if (!game->pointer_down || game->phase != H2_GARDEN_GUARD_PHASE_PLAYING)
        break;
      /* Sweeping across suns collects them. */
      (void)collect_suns_at(game, x, y);
      /* A dragged card marks the cell it would land on. */
      uint32_t lane, col;
      clear_hover(game);
      if (game->drag_from_card && game->selected != H2_GARDEN_GUARD_CARD_NONE &&
          target_cell(game, x, y, &lane, &col)) {
        game->hover_lane = (int32_t)lane;
        game->hover_col = (int32_t)col;
      }
      break;
    }
    case H2_GARDEN_GUARD_POINTER_UP: {
      uint32_t lane, col;
      /* Dragging a card onto the lawn plants where it is released. */
      if (game->pointer_down && game->drag_from_card &&
          game->phase == H2_GARDEN_GUARD_PHASE_PLAYING &&
          game->selected != H2_GARDEN_GUARD_CARD_NONE &&
          target_cell(game, x, y, &lane, &col))
        apply_card(game, lane, col);
      game->pointer_down = false;
      game->drag_from_card = false;
      clear_hover(game);
      break;
    }
    default:
      game->pointer_down = false;
      game->drag_from_card = false;
      clear_hover(game);
      break;
  }
}

static void spawn_pest(h2_garden_guard_game_t* game) {
  uint32_t left = wave_left_total(game);
  if (left == 0u) return;
  uint32_t pick = random_below(game, left);
  unsigned kind = 0;
  for (; kind < H2_GARDEN_GUARD_PEST_KIND_COUNT; ++kind) {
    if (pick < game->wave_left[kind]) break;
    pick -= game->wave_left[kind];
  }
  if (kind >= H2_GARDEN_GUARD_PEST_KIND_COUNT) return;
  h2_garden_guard_pest_t* slot = NULL;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PESTS; ++i) {
    if (game->pests[i].active) continue;
    if (slot == NULL || game->pests[i].exit_ms < slot->exit_ms)
      slot = &game->pests[i];
    if (slot->exit_ms == 0u) break;
  }
  /* A full field delays the spawn rather than dropping it. */
  if (slot == NULL) return;
  --game->wave_left[kind];
  uint32_t lane = random_below(game, H2_GARDEN_GUARD_LANES);
  if (lane == game->last_lane) lane = random_below(game, H2_GARDEN_GUARD_LANES);
  game->last_lane = lane;
  *slot = (h2_garden_guard_pest_t){
      .active = true,
      .kind = (uint8_t)kind,
      .lane = (uint8_t)lane,
      .x = PEST_ENTRY_X,
      .hp = k_pests[kind].hp,
      .armor = k_pests[kind].armor,
      .stride_ms = (uint16_t)random_below(game, 1000u)};
}

static void step_waves(h2_garden_guard_game_t* game) {
  const uint64_t now = game->time_ms;
  if (wave_left_total(game) == 0u) {
    if (game->wave >= 1u && active_pests(game) == 0u &&
        game->next_wave_ms > now + WAVE_AFTER_CLEAR_MS)
      game->next_wave_ms = now + WAVE_AFTER_CLEAR_MS;
    if (game->wave < H2_GARDEN_GUARD_WAVES && now >= game->next_wave_ms) {
      const wave_spec_t* spec = &k_waves[game->wave];
      for (unsigned k = 0; k < H2_GARDEN_GUARD_PEST_KIND_COUNT; ++k)
        game->wave_left[k] = spec->count[k];
      ++game->wave;
      game->next_spawn_ms = now + 800u;
      game->next_wave_ms = UINT64_MAX;
      if (game->wave == H2_GARDEN_GUARD_WAVES) {
        set_banner(game, "FINAL WAVE");
      } else {
        char text[H2_GARDEN_GUARD_BANNER_TEXT_MAX];
        (void)snprintf(text, sizeof(text), "WAVE %u", (unsigned)game->wave);
        set_banner(game, text);
      }
    }
    return;
  }
  if (now < game->next_spawn_ms) return;
  spawn_pest(game);
  const wave_spec_t* spec = &k_waves[game->wave - 1u];
  game->next_spawn_ms =
      now + (uint64_t)random_range(game, (float)spec->gap_min_ms,
                                   (float)spec->gap_max_ms);
  if (wave_left_total(game) == 0u)
    game->next_wave_ms = now + WAVE_AFTER_LAST_SPAWN_MS;
}

static void step_sky(h2_garden_guard_game_t* game) {
  if (game->time_ms < game->next_sky_sun_ms) return;
  game->next_sky_sun_ms =
      game->time_ms + (uint64_t)random_range(game, 9000.0f, 12000.0f);
  const h2_garden_guard_layout_t* l = &game->layout;
  const float r = h2_garden_guard_sun_radius(l);
  const float u = random_range(game, 0.06f, 0.94f);
  const float f = random_range(game, 0.08f, 0.9f);
  float x, rest;
  h2_garden_guard_ground_point(l, u, f, &x, &rest);
  /* Fall out from behind the toolbar onto the lawn. */
  add_sun(game, x, l->bar_h - r, rest);
}

static uint32_t countdown(uint32_t value, uint32_t dt) {
  return value > dt ? value - dt : 0u;
}

static void step_plants(h2_garden_guard_game_t* game, uint32_t dt) {
  const h2_garden_guard_layout_t* l = &game->layout;
  for (uint32_t lane = 0; lane < H2_GARDEN_GUARD_LANES; ++lane) {
    for (uint32_t col = 0; col < H2_GARDEN_GUARD_COLS; ++col) {
      h2_garden_guard_plant_t* plant = &game->plants[lane][col];
      if (plant->kind == H2_GARDEN_GUARD_PLANT_NONE) continue;
      plant->grow_ms = (uint16_t)countdown(plant->grow_ms, dt);
      plant->action_ms = (uint16_t)countdown(plant->action_ms, dt);
      plant->impact_ms = (uint16_t)countdown(plant->impact_ms, dt);
      if (plant->kind == H2_GARDEN_GUARD_PLANT_SUNFLOWER) {
        plant->timer_ms -= (int32_t)dt;
        if (plant->timer_ms > 0) continue;
        plant->timer_ms = (int32_t)SUNFLOWER_PERIOD_MS;
        float x, y;
        h2_garden_guard_field_point(l, lane, (float)col + 0.5f, &x, &y);
        const float size = h2_garden_guard_lane_size(l, lane);
        /* Drop from the flower head to the ground beside its stem. */
        plant->action_ms = H2_GARDEN_GUARD_BLOOM_POSE_MS;
        add_sun(game, x + 0.32f * size, y - 0.93f * size, y + 0.06f * size);
      } else if (plant->kind == H2_GARDEN_GUARD_PLANT_SHOOTER ||
                 plant->kind == H2_GARDEN_GUARD_PLANT_FROST) {
        const bool frost = plant->kind == H2_GARDEN_GUARD_PLANT_FROST;
        if (plant->timer_ms > 0) plant->timer_ms -= (int32_t)dt;
        if (plant->timer_ms > 0) continue;
        plant->timer_ms = 0;
        bool target = false;
        for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PESTS && !target; ++i) {
          const h2_garden_guard_pest_t* pest = &game->pests[i];
          target = pest->active && pest->lane == lane &&
                   pest->x >= (float)col + 0.2f && pest->x < PEST_ENTRY_X;
        }
        if (!target) continue;
        for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PEAS; ++i) {
          h2_garden_guard_pea_t* pea = &game->peas[i];
          if (pea->active) continue;
          *pea = (h2_garden_guard_pea_t){.active = true,
                                         .lane = (uint8_t)lane,
                                         .frost = frost,
                                         .x = (float)col + 0.75f};
          plant->action_ms = H2_GARDEN_GUARD_SHOT_POSE_MS;
          break;
        }
        plant->timer_ms = frost ? 1660 : SHOOTER_PERIOD_MS;
      }
    }
  }
}

static uint16_t pest_color(uint8_t kind) {
  switch (kind) {
    case H2_GARDEN_GUARD_PEST_SHELLBACK:
      return H2_GARDEN_GUARD_RGB(150, 174, 172);
    case H2_GARDEN_GUARD_PEST_RUNNER:
      return H2_GARDEN_GUARD_RGB(190, 82, 62);
    default:
      return H2_GARDEN_GUARD_RGB(124, 100, 156);
  }
}

static void kill_pest(h2_garden_guard_game_t* game,
                      h2_garden_guard_pest_t* pest, bool by_plant) {
  float x, y;
  h2_garden_guard_field_point(&game->layout, pest->lane, pest->x, &x, &y);
  y -= 0.28f * h2_garden_guard_lane_size(&game->layout, pest->lane);
  add_particles(game, x, y, 10u, pest_color(pest->kind),
                150.0f * game->layout.unit, 4.0f * game->layout.unit);
  game->score += k_pests[pest->kind].score;
  if (by_plant) game->defense_score += k_pests[pest->kind].score;
  ++game->kills;
  pest->active = false;
  pest->exit_ms = H2_GARDEN_GUARD_PEST_EXIT_MS;
}

static void step_peas(h2_garden_guard_game_t* game, float seconds) {
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PEAS; ++i) {
    h2_garden_guard_pea_t* pea = &game->peas[i];
    if (!pea->active) continue;
    pea->x += PEA_SPEED * seconds;
    if (pea->x > PEST_ENTRY_X + 0.3f) {
      pea->active = false;
      continue;
    }
    /* Hit the nearest pest whose body overlaps the pea. */
    h2_garden_guard_pest_t* hit = NULL;
    for (unsigned j = 0; j < H2_GARDEN_GUARD_MAX_PESTS; ++j) {
      h2_garden_guard_pest_t* pest = &game->pests[j];
      if (!pest->active || pest->lane != pea->lane) continue;
      if (pea->x < pest->x - 0.3f || pea->x > pest->x + 0.4f) continue;
      if (hit == NULL || pest->x < hit->x) hit = pest;
    }
    if (hit == NULL) continue;
    pea->active = false;
    float x, y;
    h2_garden_guard_field_point(&game->layout, pea->lane, pea->x, &x, &y);
    y -= H2_GARDEN_GUARD_PEA_HEIGHT *
         h2_garden_guard_lane_size(&game->layout, pea->lane);
    add_particles(game, x, y, 3u, H2_GARDEN_GUARD_RGB(140, 220, 90),
                  90.0f * game->layout.unit, 2.5f * game->layout.unit);
    hit->flash_ms = H2_GARDEN_GUARD_PEST_HIT_MS;
    if (pea->frost) hit->slow_ms = 1450u;
    if (hit->armor > 0) {
      hit->armor -= PEA_DAMAGE;
      if (hit->armor < 0) hit->armor = 0;
    } else {
      hit->hp -= PEA_DAMAGE;
      if (hit->hp <= 0) kill_pest(game, hit, true);
    }
  }
}

/* Move a sweeping cart and clear every pest it reaches. */
static void step_carts(h2_garden_guard_game_t* game, float seconds) {
  for (uint32_t lane = 0; lane < H2_GARDEN_GUARD_LANES; ++lane) {
    h2_garden_guard_cart_t* cart = &game->carts[lane];
    if (cart->state != H2_GARDEN_GUARD_CART_SWEEPING) continue;
    cart->x += CART_SPEED * seconds;
    for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PESTS; ++i) {
      h2_garden_guard_pest_t* pest = &game->pests[i];
      if (pest->active && pest->lane == lane && pest->x <= cart->x + 0.45f)
        kill_pest(game, pest, false);
    }
    if (cart->x > PEST_ENTRY_X + 0.6f) cart->state = H2_GARDEN_GUARD_CART_USED;
  }
}

/* Returns false when a pest got into the house. */
static bool step_pests(h2_garden_guard_game_t* game, uint32_t dt,
                       float seconds) {
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PESTS; ++i) {
    h2_garden_guard_pest_t* pest = &game->pests[i];
    pest->exit_ms = (uint16_t)countdown(pest->exit_ms, dt);
    if (!pest->active) continue;
    const pest_spec_t* spec = &k_pests[pest->kind];
    pest->flash_ms = (uint16_t)countdown(pest->flash_ms, dt);
    pest->slow_ms = (uint16_t)countdown(pest->slow_ms, dt);
    /* A pest chews plant column c while its center is in [c + 0.5, c + 1.15]:
     * the interval is shorter than a cell, so at most one column applies. */
    const int32_t col = (int32_t)ceilf(pest->x - 1.15f);
    h2_garden_guard_plant_t* plant = NULL;
    if (col >= 0 && col < H2_GARDEN_GUARD_COLS &&
        pest->x >= (float)col + 0.5f &&
        game->plants[pest->lane][col].kind != H2_GARDEN_GUARD_PLANT_NONE)
      plant = &game->plants[pest->lane][col];
    if (plant != NULL) {
      const uint16_t before = pest->eating ? pest->chew_ms : 0u;
      const uint16_t after = (uint16_t)(before + dt);
      pest->eating = true;
      pest->chew_ms = after % H2_GARDEN_GUARD_PEST_BITE_MS;
      /* Keep continuous combat damage; the pose marks the jaw closing. */
      if (before < 350u && after >= 350u) {
        plant->impact_ms = H2_GARDEN_GUARD_BITE_IMPACT_MS;
        if (plant->kind == H2_GARDEN_GUARD_PLANT_BRAMBLE) {
          pest->flash_ms = H2_GARDEN_GUARD_PEST_HIT_MS;
          if (pest->armor > 0)
            pest->armor = pest->armor > 24 ? pest->armor - 24 : 0;
          else
            pest->hp -= 24;
          if (pest->hp <= 0) {
            kill_pest(game, pest, true);
            continue;
          }
        }
      }
      plant->hp -= spec->bite_dps * seconds;
      if (plant->hp <= 0.0f) {
        remove_plant(game, pest->lane, (uint32_t)col,
                     H2_GARDEN_GUARD_RGB(70, 150, 60));
        pest->eating = false;
      } else {
        const uint8_t stage = plant_stage(plant);
        if (stage != plant->stage) {
          plant->stage = stage;
          ++game->plant_revision;
        }
      }
      continue;
    }
    pest->eating = false;
    pest->chew_ms = 0u;
    // All gait periods divide 8400 ms, keeping the cycle seamless at wrap.
    pest->stride_ms = (uint16_t)((pest->stride_ms + dt) % 8400u);
    pest->x -= spec->speed * seconds * (pest->slow_ms != 0u ? 0.49f : 1.0f);
    if (pest->x >= -0.15f) continue;
    h2_garden_guard_cart_t* cart = &game->carts[pest->lane];
    if (cart->state == H2_GARDEN_GUARD_CART_READY) {
      cart->state = H2_GARDEN_GUARD_CART_SWEEPING;
      cart->x = -0.4f;
    } else if (cart->state == H2_GARDEN_GUARD_CART_USED && pest->x < -0.5f) {
      return false;
    }
  }
  return true;
}

static void step_suns(h2_garden_guard_game_t* game, uint32_t dt,
                      float seconds) {
  float counter_x, counter_y;
  h2_garden_guard_sun_counter_point(&game->layout, &counter_x, &counter_y);
  const float fall = 70.0f * game->layout.unit;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_SUNS; ++i) {
    h2_garden_guard_sun_t* sun = &game->suns[i];
    if (!sun->active) continue;
    if (sun->collecting) {
      const float dx = counter_x - sun->x, dy = counter_y - sun->y;
      const float speed = sqrtf(sun->vx * sun->vx + sun->vy * sun->vy);
      if (dx * dx + dy * dy <= speed * seconds * speed * seconds) {
        sun->active = false;
        continue;
      }
      sun->x += sun->vx * seconds;
      sun->y += sun->vy * seconds;
      continue;
    }
    if (sun->y < sun->rest_y) {
      sun->y += fall * seconds;
      if (sun->y > sun->rest_y) sun->y = sun->rest_y;
      continue;
    }
    sun->age_ms += dt;
    if (sun->age_ms >= H2_GARDEN_GUARD_SUN_LIFE_MS) sun->active = false;
  }
}

static void step_particles(h2_garden_guard_game_t* game, uint32_t dt,
                           float seconds) {
  const float damping = 1.0f - fminf(seconds * 3.0f, 1.0f);
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PARTICLES; ++i) {
    h2_garden_guard_particle_t* p = &game->particles[i];
    if (!p->active) continue;
    p->age_ms = (uint16_t)(p->age_ms + dt);
    if (p->age_ms >= p->life_ms) {
      p->active = false;
      continue;
    }
    p->x += p->vx * seconds;
    p->y += p->vy * seconds;
    p->vx *= damping;
    p->vy *= damping;
  }
}

static void discover_visible_pests(h2_garden_guard_game_t* game) {
  if (game->profile.seen == H2_GARDEN_GUARD_ALL_SIGHTINGS) return;
  const h2_garden_guard_layout_t* l = &game->layout;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PESTS; ++i) {
    const h2_garden_guard_pest_t* pest = &game->pests[i];
    if (!pest->active || pest->kind >= H2_GARDEN_GUARD_PEST_KIND_COUNT)
      continue;
    const uint8_t bit = (uint8_t)(1u << pest->kind);
    if ((game->profile.seen & bit) != 0u) continue;
    float x, y;
    h2_garden_guard_field_point(l, pest->lane, pest->x, &x, &y);
    /* Wait until the face can be seen at the right edge. */
    if (x - 0.17f * h2_garden_guard_lane_size(l, pest->lane) >=
            (float)l->width ||
        y <= l->bar_h)
      continue;
    game->profile.seen |= bit;
    game->profile_dirty = true;
    game->sighting_kind = pest->kind;
    game->sighting_ms = 2200u;
  }
}

static void step_once(h2_garden_guard_game_t* game, uint32_t dt) {
  game->time_ms += dt;
  const float seconds = (float)dt * 0.001f;
  step_particles(game, dt, seconds);
  game->banner_ms = countdown(game->banner_ms, dt);
  game->sighting_ms = (uint16_t)countdown(game->sighting_ms, dt);
  if (game->phase != H2_GARDEN_GUARD_PHASE_PLAYING) return;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_CARD_COUNT; ++i) {
    game->card_cooldown_ms[i] = countdown(game->card_cooldown_ms[i], dt);
    game->card_denied_ms[i] = countdown(game->card_denied_ms[i], dt);
  }
  step_waves(game);
  discover_visible_pests(game);
  step_sky(game);
  step_plants(game, dt);
  step_peas(game, seconds);
  step_carts(game, seconds);
  if (!step_pests(game, dt, seconds)) {
    finish(game, false);
    return;
  }
  step_suns(game, dt, seconds);
  if (game->wave == H2_GARDEN_GUARD_WAVES && wave_left_total(game) == 0u &&
      active_pests(game) == 0u)
    finish(game, true);
}

void h2_garden_guard_game_step(h2_garden_guard_game_t* game, uint32_t dt_ms) {
  while (dt_ms > 0u) {
    const uint32_t dt = dt_ms > STEP_MAX_MS ? STEP_MAX_MS : dt_ms;
    step_once(game, dt);
    dt_ms -= dt;
  }
}

static void write_u32le(uint8_t* bytes, uint32_t value) {
  for (unsigned i = 0; i < 4u; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
}

static uint32_t read_u32le(const uint8_t* bytes) {
  uint32_t value = 0u;
  for (unsigned i = 0; i < 4u; ++i) value |= (uint32_t)bytes[i] << (8u * i);
  return value;
}

static uint32_t profile_checksum(const uint8_t* bytes) {
  uint32_t hash = 2166136261u;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_SAVE_BYTES - 4u; ++i)
    hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}

void h2_garden_guard_profile_encode(const h2_garden_guard_profile_t* profile,
                                    uint8_t out[H2_GARDEN_GUARD_SAVE_BYTES]) {
  memset(out, 0, H2_GARDEN_GUARD_SAVE_BYTES);
  memcpy(out, "GG01", 4u);
  write_u32le(out + 4, profile->total_score);
  write_u32le(out + 8, profile->coins);
  write_u32le(out + 12, profile->best);
  out[16] = profile->seen;
  out[17] = profile->owned;
  write_u32le(out + 20, profile_checksum(out));
}

bool h2_garden_guard_profile_decode(h2_garden_guard_profile_t* profile,
                                    const void* data, size_t size) {
  if (data == NULL || size != H2_GARDEN_GUARD_SAVE_BYTES) return false;
  const uint8_t* bytes = data;
  if (memcmp(bytes, "GG01", 4u) != 0 ||
      read_u32le(bytes + 20) != profile_checksum(bytes) ||
      (bytes[16] & ~H2_GARDEN_GUARD_ALL_SIGHTINGS) != 0 ||
      (bytes[17] & ~H2_GARDEN_GUARD_ALL_UNLOCKS) != 0)
    return false;
  *profile = (h2_garden_guard_profile_t){.total_score = read_u32le(bytes + 4),
                                         .coins = read_u32le(bytes + 8),
                                         .best = read_u32le(bytes + 12),
                                         .seen = bytes[16],
                                         .owned = bytes[17]};
  return true;
}
