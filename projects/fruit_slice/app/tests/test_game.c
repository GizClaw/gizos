#include "h2_fruit_slice_game.h"

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

static h2_fruit_slice_game_t game;
static h2_fruit_slice_game_t other;

static unsigned count_fruits(const h2_fruit_slice_game_t* g, bool menu) {
  unsigned count = 0;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i)
    if (g->fruits[i].active && g->fruits[i].menu == menu) ++count;
  return count;
}

static unsigned count_halves(const h2_fruit_slice_game_t* g) {
  unsigned count = 0;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_HALVES; ++i)
    if (g->halves[i].active) ++count;
  return count;
}

static unsigned count_particles(const h2_fruit_slice_game_t* g) {
  unsigned count = 0;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_PARTICLES; ++i)
    if (g->particles[i].active) ++count;
  return count;
}

/* A stroke through the production pointer path, one point per 8 ms. */
static void swipe(h2_fruit_slice_game_t* g, float x0, float y0, float x1,
                  float y1, unsigned steps) {
  h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_DOWN, x0, y0);
  for (unsigned i = 1; i <= steps; ++i) {
    const float t = (float)i / (float)steps;
    h2_fruit_slice_game_step(g, 8u);
    h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_MOVE,
                                x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
  }
  h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_UP, x1, y1);
}

/* Freeze the scene so slicing tests do not race gravity or spawning. */
static void clear_scene(h2_fruit_slice_game_t* g) {
  memset(g->fruits, 0, sizeof(g->fruits));
  memset(g->halves, 0, sizeof(g->halves));
  memset(g->particles, 0, sizeof(g->particles));
  memset(g->popups, 0, sizeof(g->popups));
  g->next_wave_ms = UINT64_MAX;
  g->pending_spawns = 0;
}

static h2_fruit_slice_fruit_t* place(h2_fruit_slice_game_t* g, unsigned slot,
                                     h2_fruit_slice_kind_t kind, float x,
                                     float y) {
  h2_fruit_slice_fruit_t* fruit = &g->fruits[slot];
  memset(fruit, 0, sizeof(*fruit));
  fruit->active = true;
  fruit->kind = (uint8_t)kind;
  fruit->radius = h2_fruit_slice_game_radius(g, kind);
  fruit->body.x = x;
  fruit->body.y = y;
  return fruit;
}

static void start_playing(h2_fruit_slice_game_t* g, uint32_t seed,
                          uint32_t best) {
  h2_fruit_slice_game_init(g, 480, 480, seed, best);
  swipe(g, 60.0f, 288.0f, 420.0f, 288.0f, 12);
  CHECK(g->phase == H2_FRUIT_SLICE_PHASE_PLAYING);
}

static void test_title_starts_on_menu_slice(void) {
  h2_fruit_slice_game_init(&game, 480, 480, 1u, 7u);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_TITLE);
  CHECK(game.best == 7u);
  CHECK(count_fruits(&game, true) == 1u);

  /* A tap without movement must not slice. */
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, 240.0f,
                              288.0f);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, 240.0f, 288.0f);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_TITLE);

  /* A stroke that misses the melon does nothing either. */
  swipe(&game, 20.0f, 20.0f, 460.0f, 20.0f, 10);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_TITLE);

  swipe(&game, 60.0f, 288.0f, 420.0f, 288.0f, 12);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_PLAYING);
  CHECK(game.score == 0u);
  CHECK(game.lives == H2_FRUIT_SLICE_LIVES);
  CHECK(count_fruits(&game, true) == 0u);
  CHECK(count_halves(&game) == 2u);
  CHECK(count_particles(&game) > 0u);
}

static void test_slice_scores_and_splits(void) {
  start_playing(&game, 2u, 0u);
  clear_scene(&game);
  place(&game, 0, H2_FRUIT_SLICE_APPLE, 240.0f, 240.0f);
  swipe(&game, 240.0f, 150.0f, 240.0f, 330.0f, 6);
  CHECK(game.score == 1u);
  CHECK(game.sliced_total == 1u);
  CHECK(count_fruits(&game, false) == 0u);
  CHECK(count_halves(&game) == 2u);
  /* The halves separate across the vertical cut. */
  float left = 0.0f, right = 0.0f;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_HALVES; ++i) {
    if (!game.halves[i].active) continue;
    if (game.halves[i].body.vx < 0.0f) left = game.halves[i].body.vx;
    if (game.halves[i].body.vx > 0.0f) right = game.halves[i].body.vx;
  }
  CHECK(left < 0.0f && right > 0.0f);
}

static void test_combo_awards_bonus(void) {
  start_playing(&game, 3u, 0u);
  clear_scene(&game);
  place(&game, 0, H2_FRUIT_SLICE_ORANGE, 100.0f, 240.0f);
  place(&game, 1, H2_FRUIT_SLICE_KIWI, 240.0f, 240.0f);
  place(&game, 2, H2_FRUIT_SLICE_LEMON, 380.0f, 240.0f);
  swipe(&game, 40.0f, 240.0f, 440.0f, 240.0f, 8);
  CHECK(game.score == 6u);
  bool popup = false;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_POPUPS; ++i)
    popup |=
        game.popups[i].active && strcmp(game.popups[i].text, "COMBO +3") == 0;
  CHECK(popup);

  /* Two fruits in one stroke stay a plain score. */
  clear_scene(&game);
  game.score = 0;
  place(&game, 0, H2_FRUIT_SLICE_ORANGE, 140.0f, 240.0f);
  place(&game, 1, H2_FRUIT_SLICE_KIWI, 340.0f, 240.0f);
  swipe(&game, 40.0f, 240.0f, 440.0f, 240.0f, 8);
  CHECK(game.score == 2u);
}

static void test_bomb_ends_game_and_requests_save(void) {
  start_playing(&game, 4u, 3u);
  clear_scene(&game);
  game.score = 5u;
  place(&game, 0, H2_FRUIT_SLICE_BOMB, 240.0f, 240.0f);
  swipe(&game, 150.0f, 240.0f, 330.0f, 240.0f, 6);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
  CHECK(game.flash_ms > 0u);
  CHECK(game.flash_x == game.fruits[0].body.x &&
        game.flash_y == game.fruits[0].body.y);
  CHECK(game.new_best);
  uint32_t best = 0;
  CHECK(h2_fruit_slice_game_take_best(&game, &best));
  CHECK(best == 5u);
  CHECK(!h2_fruit_slice_game_take_best(&game, &best));
  /* Bomb halves never appear. */
  CHECK(count_halves(&game) == 0u);
}

static void test_misses_cost_lives(void) {
  start_playing(&game, 5u, 7u);
  for (unsigned i = 0; i < 6000u && game.phase == H2_FRUIT_SLICE_PHASE_PLAYING;
       ++i)
    h2_fruit_slice_game_step(&game, 10u);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
  CHECK(game.lives == 0u);
  CHECK(game.missed_total == H2_FRUIT_SLICE_LIVES);
  CHECK(game.score == 0u);
  CHECK(!game.new_best);
  CHECK(!h2_fruit_slice_game_take_best(&game, NULL));
}

static void test_restart_waits_for_menu_fruit(void) {
  start_playing(&game, 6u, 0u);
  clear_scene(&game);
  place(&game, 0, H2_FRUIT_SLICE_BOMB, 240.0f, 240.0f);
  swipe(&game, 150.0f, 240.0f, 330.0f, 240.0f, 6);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
  h2_fruit_slice_game_step(&game, H2_FRUIT_SLICE_RESTART_DELAY_MS / 2u);
  CHECK(count_fruits(&game, true) == 0u);
  /* An early stroke cannot restart. */
  swipe(&game, 60.0f, 346.0f, 420.0f, 346.0f, 4);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
  h2_fruit_slice_game_step(&game, H2_FRUIT_SLICE_RESTART_DELAY_MS);
  CHECK(count_fruits(&game, true) == 1u);
  swipe(&game, 60.0f, 346.0f, 420.0f, 346.0f, 12);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_PLAYING);
  CHECK(game.score == 0u);
  CHECK(game.lives == H2_FRUIT_SLICE_LIVES);
}

static void test_trail_expires(void) {
  h2_fruit_slice_game_init(&game, 480, 480, 7u, 0u);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, 10.0f, 10.0f);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_MOVE, 30.0f, 10.0f);
  CHECK(game.trail_count == 2u);
  h2_fruit_slice_game_step(&game, H2_FRUIT_SLICE_TRAIL_LIFE_MS + 20u);
  CHECK(game.trail_count == 0u);
}

static void test_flights_stay_on_screen(void) {
  start_playing(&game, 8u, 0u);
  unsigned observed = 0;
  for (unsigned i = 0; i < 3000u; ++i) {
    h2_fruit_slice_game_step(&game, 10u);
    if (game.phase != H2_FRUIT_SLICE_PHASE_PLAYING) {
      /* Keep throwing: this test observes spawn trajectories only. */
      game.phase = H2_FRUIT_SLICE_PHASE_PLAYING;
      game.lives = H2_FRUIT_SLICE_LIVES;
    }
    for (unsigned k = 0; k < H2_FRUIT_SLICE_MAX_FRUITS; ++k) {
      const h2_fruit_slice_fruit_t* f = &game.fruits[k];
      if (!f->active || f->menu || f->body.vy >= 0.0f) continue;
      const float apex =
          f->body.y - f->body.vy * f->body.vy / (2.0f * game.gravity);
      CHECK(apex >= 0.0f);
      CHECK(apex <= 480.0f * 0.5f);
      CHECK(f->body.x > -f->radius && f->body.x < 480.0f + f->radius);
      ++observed;
    }
  }
  CHECK(observed > 100u);
}

static void test_same_seed_is_deterministic(void) {
  start_playing(&game, 9u, 0u);
  start_playing(&other, 9u, 0u);
  for (unsigned i = 0; i < 1500u; ++i) {
    h2_fruit_slice_game_step(&game, 13u);
    h2_fruit_slice_game_step(&other, 13u);
    if (i % 50u == 0u) {
      swipe(&game, 20.0f, 300.0f, 460.0f, 160.0f, 5);
      swipe(&other, 20.0f, 300.0f, 460.0f, 160.0f, 5);
    }
  }
  CHECK(game.rng == other.rng);
  CHECK(game.score == other.score);
  CHECK(game.lives == other.lives);
  CHECK(game.waves == other.waves && game.waves > 0u);
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i) {
    CHECK(game.fruits[i].active == other.fruits[i].active);
    if (game.fruits[i].active)
      CHECK(game.fruits[i].body.x == other.fruits[i].body.x &&
            game.fruits[i].body.y == other.fruits[i].body.y);
  }
}

static void test_wide_display_scales(void) {
  h2_fruit_slice_game_init(&game, 800, 480, 10u, 0u);
  CHECK(game.unit == 1.0f);
  h2_fruit_slice_game_init(&game, 240, 240, 10u, 0u);
  CHECK(game.unit == 0.5f);
  CHECK(h2_fruit_slice_game_radius(&game, H2_FRUIT_SLICE_WATERMELON) == 23.0f);
}

int main(void) {
  test_title_starts_on_menu_slice();
  test_slice_scores_and_splits();
  test_combo_awards_bonus();
  test_bomb_ends_game_and_requests_save();
  test_misses_cost_lives();
  test_restart_waits_for_menu_fruit();
  test_trail_expires();
  test_flights_stay_on_screen();
  test_same_seed_is_deterministic();
  test_wide_display_scales();
  puts("fruit slice game tests passed");
  return 0;
}
