#include "h2_garden_guard_game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                               \
  do {                                                                 \
    if (!(condition)) {                                                \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, \
              #condition);                                             \
      exit(1);                                                         \
    }                                                                  \
  } while (0)

static h2_garden_guard_game_t game;
static h2_garden_guard_game_t other;

/* A tap through the production pointer path. */
static void tap(h2_garden_guard_game_t* g, float x, float y) {
  h2_garden_guard_game_pointer(g, H2_GARDEN_GUARD_POINTER_DOWN, x, y);
  h2_garden_guard_game_pointer(g, H2_GARDEN_GUARD_POINTER_UP, x, y);
}

static void card_center(const h2_garden_guard_game_t* g, unsigned card,
                        float* x, float* y) {
  *x = g->layout.card_x[card] + g->layout.card_w * 0.5f;
  *y = g->layout.card_y + g->layout.card_h * 0.5f;
}

static void cell_center(const h2_garden_guard_game_t* g, uint32_t lane,
                        uint32_t col, float* x, float* y) {
  h2_garden_guard_field_point(&g->layout, lane, (float)col + 0.5f, x, y);
}

static void tap_card(h2_garden_guard_game_t* g, unsigned card) {
  float x, y;
  card_center(g, card, &x, &y);
  tap(g, x, y);
}

static void tap_cell(h2_garden_guard_game_t* g, uint32_t lane, uint32_t col) {
  float x, y;
  cell_center(g, lane, col, &x, &y);
  tap(g, x, y);
}

static void plant(h2_garden_guard_game_t* g, unsigned card, uint32_t lane,
                  uint32_t col) {
  tap_card(g, card);
  CHECK(g->selected == (int32_t)card);
  tap_cell(g, lane, col);
  CHECK(g->plants[lane][col].kind == card + 1u);
}

/* Hold waves and sky sun so a test controls every pest and sun. */
static void freeze_spawns(h2_garden_guard_game_t* g) {
  g->next_wave_ms = UINT64_MAX;
  g->next_sky_sun_ms = UINT64_MAX;
}

static h2_garden_guard_pest_t* place_pest(h2_garden_guard_game_t* g,
                                          unsigned slot,
                                          h2_garden_guard_pest_kind_t kind,
                                          uint8_t lane, float x) {
  h2_garden_guard_pest_t* pest = &g->pests[slot];
  memset(pest, 0, sizeof(*pest));
  pest->active = true;
  pest->kind = (uint8_t)kind;
  pest->lane = lane;
  pest->x = x;
  pest->hp = kind == H2_GARDEN_GUARD_PEST_RUNNER ? 110 : 190;
  pest->armor = kind == H2_GARDEN_GUARD_PEST_SHELLBACK ? 330 : 0;
  return pest;
}

static h2_garden_guard_sun_t* resting_sun(h2_garden_guard_game_t* g) {
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_SUNS; ++i)
    if (g->suns[i].active && !g->suns[i].collecting) return &g->suns[i];
  return NULL;
}

static void tap_action(h2_garden_guard_game_t* g,
                       h2_garden_guard_action_t action) {
  const h2_garden_guard_rect_t r =
      h2_garden_guard_action_rect(&g->layout, action);
  tap(g, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}

static void start(h2_garden_guard_game_t* g, int32_t width, int32_t height,
                  uint32_t seed) {
  h2_garden_guard_game_init(g, width, height, seed, 0u);
  CHECK(g->phase == H2_GARDEN_GUARD_PHASE_TITLE);
  tap_action(g, H2_GARDEN_GUARD_ACTION_START);
  CHECK(g->phase == H2_GARDEN_GUARD_PHASE_PLAYING);
  CHECK(g->sun == H2_GARDEN_GUARD_START_SUN);
}

static void test_cover_controls(void) {
  const int32_t extents[][2] = {{480, 320}, {368, 448}};
  for (unsigned i = 0; i < sizeof(extents) / sizeof(extents[0]); ++i) {
    h2_garden_guard_game_init(&game, extents[i][0], extents[i][1], 1u, 0u);
    const h2_garden_guard_layout_t* l = &game.layout;
    const h2_garden_guard_rect_t journal =
        h2_garden_guard_menu_tab(l, false, 0u);
    const h2_garden_guard_rect_t shop = h2_garden_guard_menu_tab(l, false, 1u);
    const h2_garden_guard_rect_t start_rect =
        h2_garden_guard_action_rect(l, H2_GARDEN_GUARD_ACTION_START);
    const h2_garden_guard_rect_t language =
        h2_garden_guard_action_rect(l, H2_GARDEN_GUARD_ACTION_LANGUAGE);
    CHECK(journal.x + journal.w < start_rect.x &&
          start_rect.x + start_rect.w < shop.x);
    CHECK(start_rect.w > journal.w && start_rect.h > shop.h);
    CHECK(language.y + language.h < shop.y);
    const h2_garden_guard_rect_t controls[] = {journal, start_rect, shop,
                                               language};
    for (unsigned j = 0; j < sizeof(controls) / sizeof(controls[0]); ++j) {
      const h2_garden_guard_rect_t r = controls[j];
      CHECK(r.x >= 0.0f && r.x + r.w <= (float)l->width);
      CHECK(r.y >= 0.0f && r.y + r.h <= (float)l->height);
    }
    tap(&game, (journal.x + journal.w + start_rect.x) * 0.5f,
        start_rect.y + 4.0f);
    CHECK(game.phase == H2_GARDEN_GUARD_PHASE_TITLE &&
          game.menu == H2_GARDEN_GUARD_MENU_NONE);
    tap(&game, journal.x + 1.0f, journal.y + 1.0f);
    CHECK(game.menu == H2_GARDEN_GUARD_MENU_JOURNAL);
    CHECK(h2_garden_guard_game_back(&game));
    tap(&game, shop.x + shop.w - 1.0f, shop.y + shop.h - 1.0f);
    CHECK(game.menu == H2_GARDEN_GUARD_MENU_SHOP);
    CHECK(h2_garden_guard_game_back(&game));
    const h2_garden_guard_language_t before = game.language;
    tap(&game, language.x + language.w - 1.0f, language.y + 1.0f);
    CHECK(game.language != before);
    tap_action(&game, H2_GARDEN_GUARD_ACTION_START);
    CHECK(game.phase == H2_GARDEN_GUARD_PHASE_PLAYING);
  }
}

static void test_layouts(void) {
  const int32_t extents[][2] = {{480, 320}, {480, 480}, {480, 800},
                                {368, 448}, {160, 160}, {1024, 600}};
  for (unsigned n = 0; n < sizeof(extents) / sizeof(extents[0]); ++n) {
    h2_garden_guard_game_init(&game, extents[n][0], extents[n][1], 1u, 0u);
    const h2_garden_guard_layout_t* l = &game.layout;
    CHECK(l->portrait == (extents[n][1] > extents[n][0]));
    CHECK(l->far_y > l->bar_h && l->near_y < l->height);
    CHECK(l->card_w >= 8.0f && l->card_h >= 8.0f);
    CHECK(l->card_x[H2_GARDEN_GUARD_CARD_COUNT - 1] + l->card_w < l->width);
    CHECK(l->card_y + l->card_h <= l->bar_h);
    for (unsigned card = 0; card < H2_GARDEN_GUARD_CARD_COUNT; ++card) {
      float cx, cy;
      card_center(&game, card, &cx, &cy);
      CHECK(h2_garden_guard_card_at(l, cx, cy) == (int32_t)card);
      CHECK(h2_garden_guard_card_at(l, cx, l->bar_h + 2.0f) ==
            H2_GARDEN_GUARD_CARD_NONE);
    }
    CHECK(h2_garden_guard_card_at(l, (float)l->width - 1.0f, l->card_y) ==
          H2_GARDEN_GUARD_CARD_NONE);
    CHECK(h2_garden_guard_lane_size(l, 4u) >
          h2_garden_guard_lane_size(l, 0u) * 1.2f);
    float left, right, y0, y1, x;
    h2_garden_guard_ground_point(l, 0.0f, 1.0f, &left, &y0);
    h2_garden_guard_ground_point(l, 1.0f, 1.0f, &right, &y1);
    CHECK(right - left < l->near_w * 0.8f);
    h2_garden_guard_ground_point(l, 0.5f, 0.8f, &x, &y1);
    const float far_gap = y1 - y0;
    h2_garden_guard_ground_point(l, 0.5f, 0.2f, &x, &y0);
    CHECK(l->near_y - y0 > far_gap * 1.5f);

    for (uint32_t lane = 0; lane < H2_GARDEN_GUARD_LANES; ++lane) {
      float lx, ly, rx, ry;
      h2_garden_guard_field_point(l, lane, 0.5f, &lx, &ly);
      h2_garden_guard_field_point(l, lane, 8.5f, &rx, &ry);
      CHECK(lx < rx && ly == ry); /* Lanes stay horizontal in portrait too. */
      for (uint32_t col = 0; col < H2_GARDEN_GUARD_COLS; ++col) {
        /* Probe all four corners just inside each slanted cell, plus its
         * centre. This catches rectangular hit boxes and reversed lanes. */
        const float offsets[] = {0.01f, 0.5f, 0.99f};
        for (unsigned a = 0; a < 3u; ++a)
          for (unsigned b = 0; b < 3u; ++b) {
            const float u = ((float)col + offsets[a]) / 9.0f;
            const float f = 1.0f - ((float)lane + offsets[b]) / 5.0f;
            float px, py, ru, rf;
            uint32_t hit_lane, hit_col;
            h2_garden_guard_ground_point(l, u, f, &px, &py);
            CHECK(h2_garden_guard_ground_at(l, px, py, &ru, &rf));
            CHECK(fabsf(ru - u) < 0.00001f && fabsf(rf - f) < 0.00001f);
            CHECK(h2_garden_guard_cell_at(l, px, py, &hit_lane, &hit_col));
            CHECK(hit_lane == lane && hit_col == col);
          }
      }
      uint32_t hit_lane, hit_col;
      h2_garden_guard_field_point(l, lane, -0.01f, &lx, &ly);
      CHECK(!h2_garden_guard_cell_at(l, lx, ly, &hit_lane, &hit_col));
      h2_garden_guard_field_point(l, lane, 9.01f, &rx, &ry);
      CHECK(!h2_garden_guard_cell_at(l, rx, ry, &hit_lane, &hit_col));
    }
    uint32_t lane, col;
    CHECK(!h2_garden_guard_cell_at(l, l->mid_x, l->far_y - 1.0f, &lane, &col));
    CHECK(!h2_garden_guard_cell_at(l, l->mid_x, l->near_y + 1.0f, &lane, &col));
  }
}

static void test_perspective_gestures(void) {
  start(&game, 480, 320, 1u);
  freeze_spawns(&game);
  float x, y, cx, cy;
  card_center(&game, H2_GARDEN_GUARD_CARD_SUNFLOWER, &cx, &cy);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_DOWN, cx, cy);
  h2_garden_guard_ground_point(&game.layout, -0.005f, 0.1f, &x, &y);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_MOVE, x, y);
  CHECK(game.hover_lane == -1 && game.hover_col == -1);
  h2_garden_guard_ground_point(&game.layout, 0.005f, 0.1f, &x, &y);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_MOVE, x, y);
  CHECK(game.hover_lane == 4 && game.hover_col == 0);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_UP, x, y);
  CHECK(game.plants[4][0].kind == H2_GARDEN_GUARD_PLANT_SUNFLOWER);
  CHECK(game.hover_lane == -1 && game.hover_col == -1);

  /* Shovel the upright head even where the ground belongs to a back row. */
  h2_garden_guard_game_step(&game, H2_GARDEN_GUARD_PLANT_GROW_MS);
  h2_garden_guard_field_point(&game.layout, 4u, 0.5f, &x, &y);
  y -= 0.9f * h2_garden_guard_lane_size(&game.layout, 4u);
  uint32_t lane, col;
  CHECK(h2_garden_guard_plant_at(&game, x, y, &lane, &col));
  CHECK(lane == 4u && col == 0u);
  tap_card(&game, H2_GARDEN_GUARD_CARD_SHOVEL);
  tap(&game, x, y);
  CHECK(game.plants[4][0].kind == H2_GARDEN_GUARD_PLANT_NONE);

  /* A falling sun hidden by the toolbar cannot steal a card press. */
  card_center(&game, H2_GARDEN_GUARD_CARD_SHOOTER, &cx, &cy);
  game.suns[0] = (h2_garden_guard_sun_t){.active = true, .x = cx, .y = cy};
  tap(&game, cx, cy);
  CHECK(game.selected == H2_GARDEN_GUARD_CARD_SHOOTER);
  CHECK(!game.suns[0].collecting);
}

static void test_title_starts_and_plants(void) {
  start(&game, 480, 320, 7u);
  freeze_spawns(&game);
  plant(&game, H2_GARDEN_GUARD_CARD_SUNFLOWER, 1u, 0u);
  CHECK(game.sun == H2_GARDEN_GUARD_START_SUN - 50u);
  CHECK(game.selected == H2_GARDEN_GUARD_CARD_NONE);
  CHECK(game.card_cooldown_ms[H2_GARDEN_GUARD_CARD_SUNFLOWER] > 0u);
  const uint32_t revision = game.plant_revision;

  /* The card is cooling down: it cannot be selected again yet. */
  tap_card(&game, H2_GARDEN_GUARD_CARD_SUNFLOWER);
  CHECK(game.selected == H2_GARDEN_GUARD_CARD_NONE);
  CHECK(game.card_denied_ms[H2_GARDEN_GUARD_CARD_SUNFLOWER] > 0u);

  /* An occupied cell keeps the selection and the sun. */
  tap_card(&game, H2_GARDEN_GUARD_CARD_PUMPKIN);
  CHECK(game.selected == H2_GARDEN_GUARD_CARD_PUMPKIN);
  tap_cell(&game, 1u, 0u);
  CHECK(game.plants[1][0].kind == H2_GARDEN_GUARD_PLANT_SUNFLOWER);
  CHECK(game.selected == H2_GARDEN_GUARD_CARD_PUMPKIN);
  CHECK(game.sun == 100u);
  /* Tapping the selected card again cancels the selection. */
  tap_card(&game, H2_GARDEN_GUARD_CARD_PUMPKIN);
  CHECK(game.selected == H2_GARDEN_GUARD_CARD_NONE);
  CHECK(game.plant_revision == revision);

  /* Dragging a card onto the lawn plants where it is released. */
  float x, y;
  card_center(&game, H2_GARDEN_GUARD_CARD_SHOOTER, &x, &y);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_DOWN, x, y);
  cell_center(&game, 3u, 2u, &x, &y);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_MOVE, x, y);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_UP, x, y);
  CHECK(game.plants[3][2].kind == H2_GARDEN_GUARD_PLANT_SHOOTER);
  CHECK(game.sun == 0u);

  /* Cooldowns end, but an unaffordable card still cannot be selected. */
  game.plants[1][0].timer_ms = 1 << 30;
  h2_garden_guard_game_step(&game, 6000u);
  tap_card(&game, H2_GARDEN_GUARD_CARD_SHOOTER);
  CHECK(game.selected == H2_GARDEN_GUARD_CARD_NONE);

  /* The shovel removes a plant without refund. */
  tap_card(&game, H2_GARDEN_GUARD_CARD_SHOVEL);
  CHECK(game.selected == H2_GARDEN_GUARD_CARD_SHOVEL);
  tap_cell(&game, 3u, 2u);
  CHECK(game.plants[3][2].kind == H2_GARDEN_GUARD_PLANT_NONE);
  CHECK(game.selected == H2_GARDEN_GUARD_CARD_NONE);
  CHECK(game.sun == 0u);

  /* A cancelled gesture does not plant on release. */
  game.sun = 100u;
  card_center(&game, H2_GARDEN_GUARD_CARD_SHOOTER, &x, &y);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_DOWN, x, y);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_CANCEL, 0.0f,
                               0.0f);
  cell_center(&game, 4u, 4u, &x, &y);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_UP, x, y);
  CHECK(game.plants[4][4].kind == H2_GARDEN_GUARD_PLANT_NONE);
  CHECK(game.sun == 100u);
}

static void test_suns(void) {
  start(&game, 480, 320, 9u);
  freeze_spawns(&game);
  plant(&game, H2_GARDEN_GUARD_CARD_SUNFLOWER, 2u, 4u);
  CHECK(game.sun == 100u);
  /* The first sun appears after the sunflower's warm-up. */
  h2_garden_guard_game_step(&game, 5900u);
  CHECK(resting_sun(&game) == NULL);
  h2_garden_guard_game_step(&game, 3000u);
  h2_garden_guard_sun_t* sun = resting_sun(&game);
  CHECK(sun != NULL && sun->y == sun->rest_y);

  /* Tapping a sun credits it at once and sends it to the counter. */
  tap(&game, sun->x, sun->y);
  CHECK(game.sun == 125u);
  CHECK(sun->collecting);
  h2_garden_guard_game_step(&game, 1500u);
  CHECK(!sun->active);
  CHECK(game.sun == 125u);
  game.plants[2][4].timer_ms = 1 << 30;

  /* Sky sun falls and is collected by sweeping across it. */
  game.next_sky_sun_ms = game.time_ms;
  h2_garden_guard_game_step(&game, 20u);
  game.next_sky_sun_ms = UINT64_MAX;
  sun = resting_sun(&game);
  CHECK(sun != NULL && sun->y < sun->rest_y);
  h2_garden_guard_game_step(&game, 8000u);
  CHECK(sun->y == sun->rest_y);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_DOWN, 1.0f,
                               sun->y);
  CHECK(!sun->collecting);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_MOVE, sun->x,
                               sun->y);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_UP, sun->x,
                               sun->y);
  CHECK(game.sun == 150u);

  /* Uncollected suns expire. */
  memset(game.suns, 0, sizeof(game.suns));
  game.next_sky_sun_ms = game.time_ms;
  h2_garden_guard_game_step(&game, 20u);
  game.next_sky_sun_ms = UINT64_MAX;
  CHECK(resting_sun(&game) != NULL);
  h2_garden_guard_game_step(&game, 8000u + H2_GARDEN_GUARD_SUN_LIFE_MS);
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_SUNS; ++i)
    CHECK(!game.suns[i].active);
  CHECK(game.sun == 150u);
}

static void test_shooter_defeats_pests(void) {
  start(&game, 480, 320, 11u);
  freeze_spawns(&game);
  plant(&game, H2_GARDEN_GUARD_CARD_SHOOTER, 0u, 1u);
  /* No target, no peas. */
  h2_garden_guard_game_step(&game, 2000u);
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PEAS; ++i)
    CHECK(!game.peas[i].active);
  /* A pest in another lane is ignored. */
  place_pest(&game, 0u, H2_GARDEN_GUARD_PEST_SHAMBLER, 1u, 8.0f);
  h2_garden_guard_game_step(&game, 100u);
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PEAS; ++i)
    CHECK(!game.peas[i].active);
  game.pests[0].active = false;

  h2_garden_guard_pest_t* pest =
      place_pest(&game, 0u, H2_GARDEN_GUARD_PEST_SHAMBLER, 0u, 8.0f);
  unsigned frames = 0;
  while (pest->active && frames < 1000u) {
    h2_garden_guard_game_step(&game, 20u);
    ++frames;
  }
  CHECK(!pest->active);
  CHECK(game.kills == 1u && game.score == 10u);
  /* Ten peas 1.4 s apart fell it long before it reaches the shooter. */
  CHECK(frames * 20u < 15000u);

  /* Shell plates absorb damage first. */
  pest = place_pest(&game, 1u, H2_GARDEN_GUARD_PEST_SHELLBACK, 0u, 6.0f);
  game.plants[0][1].timer_ms = 0;
  while (pest->armor == 330 && pest->active)
    h2_garden_guard_game_step(&game, 20u);
  CHECK(pest->armor < 330 && pest->hp == 190);
}

static void test_combat_poses_follow_events(void) {
  start(&game, 480, 320, 11u);
  freeze_spawns(&game);
  plant(&game, H2_GARDEN_GUARD_CARD_SUNFLOWER, 1u, 1u);
  plant(&game, H2_GARDEN_GUARD_CARD_SHOOTER, 2u, 2u);
  h2_garden_guard_plant_t* flower = &game.plants[1][1];
  h2_garden_guard_plant_t* shooter = &game.plants[2][2];
  CHECK(flower->grow_ms == H2_GARDEN_GUARD_PLANT_GROW_MS);
  CHECK(h2_garden_guard_plant_pose(&game, 1u, 1u).growth < 1.0f);
  CHECK(shooter->action_ms == 0u);
  h2_garden_guard_game_step(&game, H2_GARDEN_GUARD_PLANT_GROW_MS);
  CHECK(h2_garden_guard_plant_pose(&game, 1u, 1u).growth == 1.0f);
  flower->timer_ms = 1;
  h2_garden_guard_game_step(&game, 20u);
  CHECK(flower->action_ms == H2_GARDEN_GUARD_BLOOM_POSE_MS);
  CHECK(resting_sun(&game) != NULL && shooter->action_ms == 0u);
  h2_garden_guard_pest_t* pest =
      place_pest(&game, 0u, H2_GARDEN_GUARD_PEST_SHAMBLER, 2u, 3.25f);
  pest->hp = 1;
  h2_garden_guard_game_step(&game, 20u);
  CHECK(shooter->action_ms == H2_GARDEN_GUARD_SHOT_POSE_MS);
  CHECK(pest->active);
  h2_garden_guard_game_step(&game, 100u);
  CHECK(!pest->active && pest->exit_ms > 0u && game.kills == 1u);
  h2_garden_guard_game_step(&game, H2_GARDEN_GUARD_PEST_EXIT_MS);
  CHECK(pest->exit_ms == 0u && shooter->action_ms == 0u);

  /* A bite pulse requires contact and occurs when the jaw closes. */
  shooter->timer_ms = 2000;
  pest = place_pest(&game, 0u, H2_GARDEN_GUARD_PEST_SHAMBLER, 2u, 3.12f);
  h2_garden_guard_game_step(&game, 200u);
  CHECK(pest->eating && pest->chew_ms == 200u && shooter->impact_ms == 0u);
  h2_garden_guard_game_step(&game, 155u);
  CHECK(pest->eating && pest->chew_ms == 355u && shooter->impact_ms > 0u);
}

static void test_pests_chew_and_pumpkin_blocks(void) {
  start(&game, 480, 320, 13u);
  freeze_spawns(&game);
  plant(&game, H2_GARDEN_GUARD_CARD_PUMPKIN, 2u, 3u);
  h2_garden_guard_pest_t* pest =
      place_pest(&game, 0u, H2_GARDEN_GUARD_PEST_SHAMBLER, 2u, 5.0f);
  h2_garden_guard_game_step(&game, 10000u);
  /* The pest stopped at the pumpkin's edge and is chewing it. */
  CHECK(pest->active && pest->eating);
  CHECK(pest->x > 4.0f && pest->x < 4.2f);
  const float hp = game.plants[2][3].hp;
  CHECK(hp < h2_garden_guard_plant_max_hp(H2_GARDEN_GUARD_PLANT_PUMPKIN));
  const uint32_t revision = game.plant_revision;
  /* The pumpkin shows damage stages and is eventually eaten. */
  h2_garden_guard_game_step(&game, 20000u);
  CHECK(game.plants[2][3].stage >= 1u);
  CHECK(game.plant_revision != revision);
  h2_garden_guard_game_step(&game, 30000u);
  CHECK(game.plants[2][3].kind == H2_GARDEN_GUARD_PLANT_NONE);
  CHECK(!pest->eating && pest->x < 4.0f);
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_PLAYING);
}

static void test_cart_then_loss(void) {
  start(&game, 480, 320, 17u);
  freeze_spawns(&game);
  h2_garden_guard_pest_t* first =
      place_pest(&game, 0u, H2_GARDEN_GUARD_PEST_RUNNER, 4u, 0.4f);
  h2_garden_guard_pest_t* follower =
      place_pest(&game, 1u, H2_GARDEN_GUARD_PEST_SHAMBLER, 4u, 3.0f);
  /* The first pest through triggers the lane's cart, which clears it. */
  h2_garden_guard_game_step(&game, 2000u);
  CHECK(game.carts[4].state != H2_GARDEN_GUARD_CART_READY);
  CHECK(!first->active && !follower->active);
  h2_garden_guard_game_step(&game, 3000u);
  CHECK(game.carts[4].state == H2_GARDEN_GUARD_CART_USED);
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_PLAYING);
  CHECK(game.kills == 2u);

  /* With the cart spent, the next pest through ends the run. */
  place_pest(&game, 0u, H2_GARDEN_GUARD_PEST_RUNNER, 4u, 0.2f);
  for (unsigned i = 0; i < 500u && game.phase == H2_GARDEN_GUARD_PHASE_PLAYING;
       ++i)
    h2_garden_guard_game_step(&game, 20u);
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_LOST);
  CHECK(game.score == 25u && game.best == 25u && game.new_best);
  CHECK(game.earned_coins == 0u && game.profile.coins == 0u);
  uint32_t best = 0;
  CHECK(h2_garden_guard_game_take_best(&game, &best) && best == 25u);
  CHECK(!h2_garden_guard_game_take_best(&game, &best));
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PESTS; ++i)
    CHECK(!game.pests[i].active);

  /* Restart waits for the delay, then starts a fresh garden. */
  tap_action(&game, H2_GARDEN_GUARD_ACTION_RESULT_AGAIN);
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_LOST);
  h2_garden_guard_game_step(&game, H2_GARDEN_GUARD_RESTART_DELAY_MS);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_RESULT_AGAIN);
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_PLAYING);
  CHECK(game.sun == H2_GARDEN_GUARD_START_SUN && game.score == 0u);
  CHECK(game.carts[4].state == H2_GARDEN_GUARD_CART_READY);
  CHECK(game.best == 25u && !game.new_best);
}

/* Defend every lane and run the whole schedule to its end. */
static void play_to_end(h2_garden_guard_game_t* g, uint32_t* waves_seen) {
  for (uint32_t lane = 0; lane < H2_GARDEN_GUARD_LANES; ++lane)
    for (uint32_t col = 0; col < 3u; ++col)
      g->plants[lane][col] = (h2_garden_guard_plant_t){
          .kind = H2_GARDEN_GUARD_PLANT_SHOOTER, .hp = 300.0f};
  uint32_t seen = 0;
  for (unsigned i = 0; i < 20000u && g->phase == H2_GARDEN_GUARD_PHASE_PLAYING;
       ++i) {
    h2_garden_guard_game_step(g, 50u);
    if (g->wave > seen) seen = g->wave;
  }
  *waves_seen = seen;
}

static void test_waves_to_victory(void) {
  start(&game, 480, 480, 23u);
  CHECK(game.wave == 0u);
  CHECK(h2_garden_guard_pests_remaining(&game) ==
        h2_garden_guard_pests_total());
  /* Nothing arrives during the ready time. */
  h2_garden_guard_game_step(&game, H2_GARDEN_GUARD_FIRST_WAVE_MS - 100u);
  CHECK(game.wave == 0u);
  h2_garden_guard_game_step(&game, 200u);
  CHECK(game.wave == 1u && strcmp(game.banner, "WAVE 1") == 0);

  uint32_t waves = 0;
  play_to_end(&game, &waves);
  CHECK(waves == H2_GARDEN_GUARD_WAVES);
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_WON);
  CHECK(game.kills == h2_garden_guard_pests_total());
  CHECK(h2_garden_guard_pests_remaining(&game) == 0u);
  CHECK(game.score > H2_GARDEN_GUARD_WIN_BONUS && game.new_best);

  /* The schedule is deterministic for a seed. */
  start(&other, 480, 480, 23u);
  h2_garden_guard_game_step(&other, H2_GARDEN_GUARD_FIRST_WAVE_MS + 100u);
  play_to_end(&other, &waves);
  CHECK(other.phase == H2_GARDEN_GUARD_PHASE_WON);
  CHECK(other.score == game.score && other.time_ms == game.time_ms);
}

static void test_portrait_play(void) {
  start(&game, 368, 448, 29u);
  CHECK(game.layout.portrait);
  freeze_spawns(&game);
  plant(&game, H2_GARDEN_GUARD_CARD_SHOOTER, 2u, 0u);
  h2_garden_guard_pest_t* pest =
      place_pest(&game, 0u, H2_GARDEN_GUARD_PEST_RUNNER, 2u, 7.0f);
  for (unsigned i = 0; i < 1000u && pest->active; ++i)
    h2_garden_guard_game_step(&game, 20u);
  CHECK(!pest->active && game.kills == 1u);
}

static void tap_rect(h2_garden_guard_game_t* g, h2_garden_guard_rect_t r) {
  tap(g, r.x + 0.5f * r.w, r.y + 0.5f * r.h);
}

static void test_profile_and_collection(void) {
  const h2_garden_guard_profile_t source = {
      .total_score = 0x01020304u,
      .coins = 55u,
      .best = 123u,
      .seen = H2_GARDEN_GUARD_ALL_SIGHTINGS,
      .owned = H2_GARDEN_GUARD_FROST_BIT};
  uint8_t bytes[H2_GARDEN_GUARD_SAVE_BYTES];
  h2_garden_guard_profile_encode(&source, bytes);
  CHECK(bytes[4] == 4u && bytes[7] == 1u);
  h2_garden_guard_profile_t loaded = {0};
  CHECK(h2_garden_guard_profile_decode(&loaded, bytes, sizeof(bytes)));
  CHECK(loaded.total_score == source.total_score && loaded.coins == 55u &&
        loaded.best == 123u && loaded.seen == 7u && loaded.owned == 1u);
  CHECK(!h2_garden_guard_profile_decode(&loaded, bytes, sizeof(bytes) - 1u));
  bytes[7] ^= 1u;
  CHECK(!h2_garden_guard_profile_decode(&loaded, bytes, sizeof(bytes)));
  CHECK(loaded.total_score == source.total_score);

  start(&game, 480, 800, 3u);
  freeze_spawns(&game);
  h2_garden_guard_pest_t* pest =
      place_pest(&game, 0u, H2_GARDEN_GUARD_PEST_SHAMBLER, 4u, 10.7f);
  h2_garden_guard_game_step(&game, 20u);
  CHECK(game.profile.seen == 0u && !game.profile_dirty);
  pest->x = 9.38f;
  h2_garden_guard_game_step(&game, 20u);
  CHECK(game.profile.seen == 1u && game.profile_dirty &&
        game.sighting_ms != 0u);
  CHECK(pest->active && game.kills == 0u);
  game.profile_dirty = false;
  place_pest(&game, 1u, H2_GARDEN_GUARD_PEST_SHAMBLER, 1u, 9.41f);
  h2_garden_guard_game_step(&game, 20u);
  CHECK(!game.profile_dirty && game.profile.seen == 1u);
  place_pest(&game, 2u, H2_GARDEN_GUARD_PEST_SHELLBACK, 2u, 9.41f);
  place_pest(&game, 3u, H2_GARDEN_GUARD_PEST_RUNNER, 3u, 9.42f);
  h2_garden_guard_game_step(&game, 20u);
  CHECK(game.profile.seen == 7u && game.profile_dirty && game.kills == 0u);
}

static void test_shop_and_earned_coins(void) {
  h2_garden_guard_game_init(&game, 368, 448, 13u, 88u);
  const h2_garden_guard_layout_t* l = &game.layout;
  tap_rect(&game, h2_garden_guard_menu_tab(l, false, 1u));
  CHECK(game.menu == H2_GARDEN_GUARD_MENU_SHOP);
  const h2_garden_guard_rect_t frost = h2_garden_guard_menu_row(l, 0u);
  const h2_garden_guard_rect_t bramble = h2_garden_guard_menu_row(l, 1u);
  tap(&game, frost.x + frost.w * 0.9f, frost.y + frost.h * 0.5f);
  CHECK(game.profile.coins == 0u && game.profile.owned == 0u &&
        !game.profile_dirty);
  game.profile.coins = 18u;
  tap(&game, frost.x + frost.w * 0.9f, frost.y + frost.h * 0.5f);
  tap(&game, frost.x + frost.w * 0.9f, frost.y + frost.h * 0.5f);
  CHECK(game.profile.coins == 12u && game.profile.owned == 1u);
  tap(&game, bramble.x + bramble.w * 0.9f, bramble.y + bramble.h * 0.5f);
  CHECK(game.profile.coins == 0u && game.profile.owned == 3u);
  tap_rect(&game, h2_garden_guard_menu_back(l));
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_TITLE &&
        game.menu == H2_GARDEN_GUARD_MENU_NONE);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_START);
  freeze_spawns(&game);
  game.sun = 500u;
  tap_card(&game, H2_GARDEN_GUARD_CARD_FROST);
  tap_cell(&game, 1u, 2u);
  tap_card(&game, H2_GARDEN_GUARD_CARD_BRAMBLE);
  tap_cell(&game, 2u, 4u);
  CHECK(game.plants[1][2].kind == H2_GARDEN_GUARD_PLANT_FROST &&
        game.sun == 300u);
  CHECK(game.plants[2][4].kind == H2_GARDEN_GUARD_PLANT_BRAMBLE);
  h2_garden_guard_pest_t* frozen =
      place_pest(&game, 0u, H2_GARDEN_GUARD_PEST_SHAMBLER, 1u, 4.4f);
  h2_garden_guard_pest_t* thorned =
      place_pest(&game, 1u, H2_GARDEN_GUARD_PEST_SHAMBLER, 2u, 5.1f);
  thorned->hp = 20;
  h2_garden_guard_game_step(&game, 420u);
  CHECK(frozen->slow_ms != 0u && frozen->hp == 170);
  CHECK(!thorned->active && game.defense_score == 10u);
  tap_card(&game, H2_GARDEN_GUARD_CARD_SHOVEL);
  tap_cell(&game, 2u, 4u);
  CHECK(game.plants[2][4].kind == H2_GARDEN_GUARD_PLANT_NONE);
  game.carts[3].state = H2_GARDEN_GUARD_CART_USED;
  place_pest(&game, 2u, H2_GARDEN_GUARD_PEST_SHAMBLER, 3u, -0.51f);
  h2_garden_guard_game_step(&game, 20u);
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_LOST);
  CHECK(game.profile.coins == 3u && game.earned_coins == 3u);
  CHECK(game.profile.total_score == 10u && game.profile.best == 88u);
  tap_rect(&game, h2_garden_guard_menu_tab(l, true, 0u));
  CHECK(game.menu == H2_GARDEN_GUARD_MENU_JOURNAL);
  h2_garden_guard_game_step(&game, H2_GARDEN_GUARD_RESTART_DELAY_MS + 300u);
  tap_rect(&game, h2_garden_guard_menu_back(l));
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_LOST && game.profile.coins == 3u);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_RESULT_AGAIN);
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_PLAYING && game.earned_coins == 0u);
  CHECK(game.profile.coins == 3u && game.profile.owned == 3u);
}

static void test_pause_and_language(void) {
  start(&game, 480, 800, 23u);
  freeze_spawns(&game);
  plant(&game, H2_GARDEN_GUARD_CARD_SUNFLOWER, 1u, 1u);
  h2_garden_guard_pest_t* pest =
      place_pest(&game, 0u, H2_GARDEN_GUARD_PEST_SHAMBLER, 1u, 2.0f);
  h2_garden_guard_game_step(&game, 30u);
  const uint64_t time = game.time_ms;
  const uint64_t spawn = game.next_wave_ms;
  const float hp = game.plants[1][1].hp;
  const uint16_t chew = pest->chew_ms;
  const uint32_t cooldown = game.card_cooldown_ms[0];
  const uint32_t sun = game.sun;
  game.profile.coins = 4u;
  game.profile.total_score = 89u;
  tap_card(&game, H2_GARDEN_GUARD_CARD_SHOVEL);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_PAUSE);
  CHECK(h2_garden_guard_paused(&game) &&
        game.selected == H2_GARDEN_GUARD_CARD_NONE);
  tap_cell(&game, 1u, 1u);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_PAUSE_LANGUAGE);
  CHECK(game.language == H2_GARDEN_GUARD_LANG_ZH && game.language_dirty);
  h2_garden_guard_game_step(&game, 30000u);
  CHECK(game.time_ms == time && game.next_wave_ms == spawn &&
        pest->chew_ms == chew);
  CHECK(game.plants[1][1].hp == hp && game.card_cooldown_ms[0] == cooldown &&
        game.sun == sun);

  /* A single gesture cannot open confirmation and then accept it. */
  const h2_garden_guard_rect_t leave =
      h2_garden_guard_action_rect(&game.layout, H2_GARDEN_GUARD_ACTION_LEAVE);
  const h2_garden_guard_rect_t confirm =
      h2_garden_guard_action_rect(&game.layout, H2_GARDEN_GUARD_ACTION_CONFIRM);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_DOWN,
                               leave.x + leave.w / 2, leave.y + leave.h * 0.9f);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_UP,
                               confirm.x + confirm.w / 2, confirm.y + 1.0f);
  CHECK(game.menu == H2_GARDEN_GUARD_MENU_PAUSE);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_LEAVE);
  CHECK(game.menu == H2_GARDEN_GUARD_MENU_CONFIRM);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_CANCEL);
  CHECK(game.menu == H2_GARDEN_GUARD_MENU_PAUSE);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_RESUME);
  CHECK(!h2_garden_guard_paused(&game) && game.time_ms == time);
  h2_garden_guard_game_step(&game, 30u);
  CHECK(game.time_ms == time + 30u && pest->chew_ms > chew);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_PAUSE);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_LEAVE);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_CONFIRM);
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_TITLE &&
        game.menu == H2_GARDEN_GUARD_MENU_NONE);
  CHECK(game.profile.total_score == 89u && game.profile.coins == 4u &&
        game.earned_coins == 0u);
  CHECK(game.language == H2_GARDEN_GUARD_LANG_ZH && !pest->active);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_LANGUAGE);
  CHECK(game.language == H2_GARDEN_GUARD_LANG_EN &&
        game.phase == H2_GARDEN_GUARD_PHASE_TITLE);
  tap_action(&game, H2_GARDEN_GUARD_ACTION_START);
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_PLAYING &&
        game.sun == H2_GARDEN_GUARD_START_SUN);
}

static void test_journal_focus(void) {
  static const int sizes[][2] = {{480, 320}, {480, 800}, {160, 160}};
  for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    h2_garden_guard_game_init(&game, sizes[i][0], sizes[i][1], 13u, 0u);
    game.profile.seen = 5u;
    const h2_garden_guard_layout_t* l = &game.layout;
    const h2_garden_guard_rect_t first =
        h2_garden_guard_journal_card_rect(l, 0u);
    const h2_garden_guard_rect_t locked =
        h2_garden_guard_journal_card_rect(l, 1u);
    const h2_garden_guard_rect_t last =
        h2_garden_guard_journal_card_rect(l, 2u);
    const h2_garden_guard_rect_t detail =
        h2_garden_guard_journal_detail_rect(l);
    const h2_garden_guard_rect_t close = h2_garden_guard_journal_close_rect(l);
    CHECK(first.x >= 0.0f && last.x + last.w <= (float)l->width);
    CHECK(detail.y >= 0.0f && detail.y + detail.h <= (float)l->height);
    CHECK(close.x >= detail.x && close.x + close.w <= detail.x + detail.w);
    tap_rect(&game, h2_garden_guard_menu_tab(l, false, 0u));
    tap_rect(&game, locked);
    CHECK(game.menu == H2_GARDEN_GUARD_MENU_JOURNAL && game.journal_card == 0u);
    tap_rect(&game, first);
    CHECK(game.journal_card == 1u && h2_garden_guard_journal_animating(&game));
    tap_rect(&game, last);
    tap_rect(&game, close);
    CHECK(game.journal_card == 1u && !game.journal_closing);
    h2_garden_guard_game_step(&game, H2_GARDEN_GUARD_JOURNAL_MS);
    CHECK(!h2_garden_guard_journal_animating(&game));
    tap_rect(&game, close);
    CHECK(game.journal_closing);
    tap_rect(&game, h2_garden_guard_menu_back(l));
    CHECK(game.menu == H2_GARDEN_GUARD_MENU_JOURNAL);
    h2_garden_guard_game_step(&game, H2_GARDEN_GUARD_JOURNAL_MS);
    CHECK(game.journal_card == 0u && !h2_garden_guard_journal_animating(&game));
    game.reduce_motion = true;
    tap_rect(&game, last);
    CHECK(game.journal_card == 3u && !h2_garden_guard_journal_animating(&game));
    CHECK(h2_garden_guard_game_back(&game) && game.journal_card == 0u);
    CHECK(h2_garden_guard_game_back(&game) &&
          game.menu == H2_GARDEN_GUARD_MENU_NONE);
    CHECK(game.profile.seen == 5u && !game.profile_dirty);
  }
}

int main(void) {
  test_journal_focus();
  test_pause_and_language();
  test_profile_and_collection();
  test_shop_and_earned_coins();
  test_cover_controls();
  test_layouts();
  test_perspective_gestures();
  test_title_starts_and_plants();
  test_suns();
  test_shooter_defeats_pests();
  test_combat_poses_follow_events();
  test_pests_chew_and_pumpkin_blocks();
  test_cart_then_loss();
  test_waves_to_victory();
  test_portrait_play();
  puts("garden guard game tests passed");
  return 0;
}
