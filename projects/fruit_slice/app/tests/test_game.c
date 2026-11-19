#include "h2_fruit_slice_game.h"

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
        game.popups[i].active && strcmp(game.popups[i].text, "连击 +3") == 0;
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

static void dismiss_cards(h2_fruit_slice_game_t* g) {
  for (unsigned n = 0; n < H2_FRUIT_SLICE_CARD_COUNT && g->cards.reveal_queue;
       ++n) {
    h2_fruit_slice_game_step(g, 2000u);
    h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_DOWN, 240, 200);
    h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_UP, 240, 200);
  }
  CHECK(!g->cards.reveal_active && g->cards.reveal_queue == 0u);
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
  CHECK(game.cards.reveal_active);
  dismiss_cards(&game);
  h2_fruit_slice_game_step(&game, H2_FRUIT_SLICE_RESTART_DELAY_MS);
  CHECK(count_fruits(&game, true) == 1u);
  swipe(&game, 60.0f, 346.0f, 420.0f, 346.0f, 12);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_PLAYING);
  CHECK(game.score == 0u);
  CHECK(game.lives == H2_FRUIT_SLICE_LIVES);
}

static void test_trail_expires(void) {
  h2_fruit_slice_game_init(&game, 480, 480, 7u, 0u);
  /* Stroke below the title controls. */
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, 10.0f, 60.0f);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_MOVE, 30.0f, 60.0f);
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

static void tap_button(h2_fruit_slice_game_t* g, uint32_t id) {
  h2_fruit_slice_rect_t r;
  CHECK(h2_fruit_slice_game_button_rect(g, id, &r));
  const float x = (r.x0 + r.x1) * 0.5f, y = (r.y0 + r.y1) * 0.5f;
  h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
  h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_UP, x, y);
}

static void test_tasks_and_shop(void) {
  start_playing(&game, 12u, 0u);
  /* Three real combos plus a single fruit satisfy both repeatable tasks. */
  for (unsigned i = 0; i < 3u; ++i) {
    clear_scene(&game);
    for (unsigned k = 0; k < 3u; ++k)
      place(&game, k, H2_FRUIT_SLICE_APPLE, 100.0f + (float)k * 140.0f, 240.0f);
    swipe(&game, 40.0f, 240.0f, 440.0f, 240.0f, 8);
  }
  clear_scene(&game);
  place(&game, 0, H2_FRUIT_SLICE_APPLE, 240.0f, 240.0f);
  swipe(&game, 40.0f, 240.0f, 440.0f, 240.0f, 8);
  CHECK(game.progress.coins == 0u);
  CHECK(game.progress.fruit_credit == 10u && game.progress.combo_credit == 3u);
  CHECK(game.score == 19u);
  /* Cross the one-shot score target via gameplay, then finish on a bomb. */
  clear_scene(&game);
  place(&game, 0, H2_FRUIT_SLICE_APPLE, 240.0f, 240.0f);
  swipe(&game, 40.0f, 240.0f, 440.0f, 240.0f, 8);
  CHECK(game.progress.score_task == 1u);
  clear_scene(&game);
  place(&game, 0, H2_FRUIT_SLICE_BOMB, 240.0f, 240.0f);
  swipe(&game, 40.0f, 240.0f, 440.0f, 240.0f, 8);
  dismiss_cards(&game);
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_TASKS);
  const uint64_t paused = game.time_ms;
  h2_fruit_slice_game_step(&game, 2000u);
  CHECK(game.time_ms == paused);
  for (unsigned i = 0; i < 3u; ++i) {
    tap_button(&game, H2_FRUIT_SLICE_BUTTON_CLAIM + i);
    const uint32_t coins = game.progress.coins;
    tap_button(&game, H2_FRUIT_SLICE_BUTTON_CLAIM + i);
    CHECK(game.progress.coins == coins);
  }
  CHECK(game.progress.coins == 80u);
  CHECK(game.progress.fruit_credit == 1u && game.progress.combo_credit == 0u);
  CHECK(game.progress.score_task == 2u && game.progress_dirty);
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_BACK);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_SHOP);
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + 2u);
  CHECK(game.progress.coins == 80u && game.progress.owned == 1u);
  CHECK(game.progress.equipped == 0u); /* Insufficient funds cannot equip. */
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
  CHECK(game.progress.coins == 40u && game.progress.owned == 3u);
  CHECK(game.progress.equipped == 1u);
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
  CHECK(game.progress.coins == 40u); /* Already-owned gear never charges. */
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_WEAPON);
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
  CHECK(game.progress.equipped == 1u && game.progress.coins == 40u);
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_BACK);
  h2_fruit_slice_game_step(&game, H2_FRUIT_SLICE_RESTART_DELAY_MS);
  swipe(&game, 60.0f, 346.0f, 420.0f, 346.0f, 12);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_PLAYING && game.score == 0u);
  CHECK(game.progress.coins == 40u && game.progress.equipped == 1u);
}

static void test_progress_format_and_input(void) {
  h2_fruit_slice_game_init(&game, 480, 480, 14u, 30u);
  CHECK(game.progress.owned == 1u && game.progress.coins == 0u);
  CHECK(game.progress.score_task == 1u); /* Existing best survives upgrade. */
  game.progress.coins = 90u;
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_SHOP);
  h2_fruit_slice_rect_t r;
  CHECK(h2_fruit_slice_game_button_rect(&game,
                                        H2_FRUIT_SLICE_BUTTON_WEAPON + 2u, &r));
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, r.x0 + 1.0f,
                              r.y0 + 1.0f);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_CANCEL, r.x0 + 1.0f,
                              r.y0 + 1.0f);
  CHECK(game.progress.coins == 90u && game.progress.owned == 1u);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, r.x0 + 1.0f,
                              r.y0 + 1.0f);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, 0.0f, 0.0f);
  CHECK(game.progress.coins == 90u);
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + 2u);
  CHECK(game.progress.coins == 0u && game.progress.equipped == 2u);
  uint8_t bytes[H2_FRUIT_SLICE_PROGRESS_BYTES];
  h2_fruit_slice_progress_encode(&game.progress, bytes);
  h2_fruit_slice_game_init(&other, 480, 480, 15u, 30u);
  CHECK(h2_fruit_slice_progress_decode(&other.progress, bytes, sizeof(bytes)));
  CHECK(other.progress.owned == 5u && other.progress.equipped == 2u);
  CHECK(other.progress.score_task == 1u);
  CHECK(!h2_fruit_slice_progress_decode(&other.progress, bytes,
                                        sizeof(bytes) - 1u));
  bytes[4] = 3u; /* Future save version. */
  CHECK(!h2_fruit_slice_progress_decode(&other.progress, bytes, sizeof(bytes)));
  bytes[4] = 2u;
  bytes[24] = 1u; /* Equipped axe is not owned. */
  CHECK(!h2_fruit_slice_progress_decode(&other.progress, bytes, sizeof(bytes)));
  CHECK(other.progress.owned == 5u && other.progress.equipped == 2u);
  h2_fruit_slice_progress_encode(&game.progress, bytes);
  memset(bytes + 8, 255, 4u); /* Overflowed currency is rejected. */
  CHECK(!h2_fruit_slice_progress_decode(&other.progress, bytes, sizeof(bytes)));
}

static void test_weapon_reach(void) {
  for (unsigned id = 0; id < 3u; ++id) {
    start_playing(&game, 16u, 0u);
    clear_scene(&game);
    game.progress.owned = 7u;
    game.progress.equipped = id;
    place(&game, 0, H2_FRUIT_SLICE_APPLE, 240.0f, 240.0f);
    swipe(&game, 40.0f, 279.0f, 440.0f, 279.0f, 1);
    CHECK(game.score == (id == 0u ? 0u : 1u));
    clear_scene(&game);
    place(&game, 0, H2_FRUIT_SLICE_BOMB, 240.0f, 240.0f);
    swipe(&game, 40.0f, 286.0f, 440.0f, 286.0f, 1);
    CHECK((game.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER) == (id == 2u));
  }
}

static void test_progress_limits(void) {
  h2_fruit_slice_game_init(&game, 480, 480, 24u, 0u);
  game.progress.coins = H2_FRUIT_SLICE_PROGRESS_LIMIT;
  game.progress.fruit_credit = 30u;
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_TASKS);
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_CLAIM);
  CHECK(game.progress.coins == H2_FRUIT_SLICE_PROGRESS_LIMIT);
  CHECK(game.progress.fruit_credit == 30u && !game.progress_dirty);
  game.progress.coins = H2_FRUIT_SLICE_PROGRESS_LIMIT - 15u;
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_CLAIM);
  CHECK(game.progress.coins == H2_FRUIT_SLICE_PROGRESS_LIMIT);
  CHECK(game.progress.fruit_credit == 20u);
  game.progress.coins = 0u;
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_CLAIM);
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_CLAIM);
  tap_button(&game, H2_FRUIT_SLICE_BUTTON_CLAIM);
  CHECK(game.progress.coins == 30u && game.progress.fruit_credit == 0u);
}

static void test_variety_ids_and_hits(void) {
  CHECK(H2_FRUIT_SLICE_BOMB == 5 && H2_FRUIT_SLICE_STRAWBERRY == 6);
  CHECK(H2_FRUIT_SLICE_CARD_FRUIT_SALAD == 5 && H2_FRUIT_SLICE_CARD_BOMB == 9);
  CHECK(H2_FRUIT_SLICE_FRUIT_KINDS == 9u);
  for (unsigned i = 0; i < H2_FRUIT_SLICE_FRUIT_KINDS; ++i)
    CHECK(h2_fruit_slice_is_fruit(h2_fruit_slice_kind_at(i)));
  CHECK(h2_fruit_slice_kind_at(9u) == H2_FRUIT_SLICE_KIND_COUNT);
  CHECK(!h2_fruit_slice_is_fruit(H2_FRUIT_SLICE_BOMB));
  CHECK(!h2_fruit_slice_is_fruit(255u));
  const unsigned sizes[][2] = {{480u, 320u}, {160u, 160u}};
  for (unsigned size = 0; size < 2u; ++size) {
    const float w = (float)sizes[size][0], h = (float)sizes[size][1];
    for (unsigned k = 6u; k < H2_FRUIT_SLICE_KIND_COUNT; ++k) {
      for (unsigned turn = 0u; turn < 8u; ++turn) {
        h2_fruit_slice_game_init(&game, (int32_t)w, (int32_t)h, 42u, 0u);
        game.phase = H2_FRUIT_SLICE_PHASE_PLAYING;
        h2_fruit_slice_cards_run_start(&game.cards);
        clear_scene(&game);
        h2_fruit_slice_fruit_t* f =
            place(&game, 0u, (h2_fruit_slice_kind_t)k, w * 0.5f, h * 0.5f);
        const float a = (float)turn * H2_FRUIT_SLICE_PI / 4.0f;
        f->body.angle = a;
        const float radius = f->radius;
        /* A short stroke through edible flesh, rotated with the body. */
        const float ly = k == H2_FRUIT_SLICE_BANANA ? 0.20f : 0.0f;
        const float cx = f->body.x - sinf(a) * ly * radius;
        const float cy = f->body.y + cosf(a) * ly * radius;
        h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN,
                                    cx - cosf(a) * radius * 0.2f,
                                    cy - sinf(a) * radius * 0.2f);
        h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP,
                                    cx + cosf(a) * radius * 0.2f,
                                    cy + sinf(a) * radius * 0.2f);
        CHECK(game.sliced_total == 1u && game.score == 1u);
        CHECK(game.progress.fruit_credit == 1u && game.cards.run_slices == 1u);
        CHECK(count_halves(&game) == 2u && !f->active);
        CHECK(fabsf(game.halves[0].shape_angle + game.halves[0].body.angle -
                    a) < 0.001f);
      }
    }
    /* The banana tips are edible; the obvious notch must remain empty. */
    const float samples[][3] = {{-0.87f, -0.23f, 1.0f},
                                {0.87f, -0.23f, 1.0f},
                                {0.0f, -0.42f, 0.0f},
                                {0.0f, 0.70f, 0.0f}};
    for (unsigned s = 0; s < 4u; ++s) {
      h2_fruit_slice_game_init(&game, (int32_t)w, (int32_t)h, 42u, 0u);
      game.phase = H2_FRUIT_SLICE_PHASE_PLAYING;
      clear_scene(&game);
      h2_fruit_slice_fruit_t* f =
          place(&game, 0u, H2_FRUIT_SLICE_BANANA, w / 2, h / 2);
      const float x = f->body.x + f->radius * samples[s][0];
      const float y = f->body.y + f->radius * samples[s][1];
      h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, x,
                                  y - game.unit * 2);
      h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, x,
                                  y + game.unit * 2);
      CHECK(game.sliced_total == (unsigned)samples[s][2]);
    }
    h2_fruit_slice_game_init(&game, (int32_t)w, (int32_t)h, 42u, 0u);
    game.phase = H2_FRUIT_SLICE_PHASE_PLAYING;
    clear_scene(&game);
    h2_fruit_slice_fruit_t* p =
        place(&game, 0u, H2_FRUIT_SLICE_PINEAPPLE, w / 2, h / 2);
    const float y = p->body.y - p->radius * 0.85f;
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN,
                                w / 2 - 3 * game.unit, y);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP,
                                w / 2 + 3 * game.unit, y);
    CHECK(p->active && game.sliced_total == 0u);
  }
  /* Two bunches in one stroke still produce two events, never ten berries. */
  start_playing(&game, 42u, 0u);
  clear_scene(&game);
  place(&game, 0u, H2_FRUIT_SLICE_GRAPES, 180, 220);
  place(&game, 1u, H2_FRUIT_SLICE_GRAPES, 300, 220);
  swipe(&game, 100, 220, 380, 220, 1u);
  CHECK(game.sliced_total == 2u && game.score == 2u &&
        count_halves(&game) == 4u);
}

static void test_natural_variety(void) {
  unsigned seen = 0u, sliced = 0u, opening_strawberries = 0u;
  unsigned counts[H2_FRUIT_SLICE_KIND_COUNT] = {0};
  for (unsigned seed = 1u; seed <= 16u; ++seed) {
    start_playing(&game, seed, 0u);
    /* Keep normal spawn/flight/score paths. Slice visible fruit at its body;
     * discard bombs only after counting them, so the deterministic run lasts.
     */
    for (unsigned frame = 0u; frame < 3000u; ++frame) {
      h2_fruit_slice_game_step(&game, 16u);
      for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i) {
        if (game.fruits[i].active &&
            game.fruits[i].kind == H2_FRUIT_SLICE_BOMB) {
          ++counts[H2_FRUIT_SLICE_BOMB];
          game.fruits[i].active = false;
        }
      }
      for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i) {
        h2_fruit_slice_fruit_t* f = &game.fruits[i];
        if (!f->active || f->menu || f->body.y > 360.0f) continue;
        ++counts[f->kind];
        seen |= 1u << f->kind;
        if (game.waves <= 2u && f->kind == H2_FRUIT_SLICE_STRAWBERRY)
          ++opening_strawberries;
        if (f->kind == H2_FRUIT_SLICE_PINEAPPLE)
          CHECK(fabsf(f->body.spin) <= 0.66f);
        const float offset =
            f->kind == H2_FRUIT_SLICE_BANANA ? f->radius * 0.2f : 0.0f;
        const float y = f->body.y + cosf(f->body.angle) * offset;
        const float x = f->body.x - sinf(f->body.angle) * offset;
        h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, x - 2,
                                    y);
        h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, x + 2, y);
        CHECK(!f->active);
        sliced |= 1u << f->kind;
      }
    }
  }
  CHECK(opening_strawberries > 0u && counts[H2_FRUIT_SLICE_BOMB] > 0u);
  for (unsigned i = 0; i < H2_FRUIT_SLICE_FRUIT_KINDS; ++i) {
    const unsigned kind = h2_fruit_slice_kind_at(i);
    CHECK((seen & sliced & (1u << kind)) != 0u && counts[kind] >= 20u);
    printf("natural fruit kind=%u count=%u\n", kind, counts[kind]);
  }
}

static bool rects_overlap(h2_fruit_slice_rect_t a, h2_fruit_slice_rect_t b) {
  return a.x0 < b.x1 && a.x1 > b.x0 && a.y0 < b.y1 && a.y1 > b.y0;
}

static void test_menu_layout_and_tap_routes(void) {
  const int32_t sizes[][2] = {{160, 160}, {240, 240}, {480, 320}, {368, 448},
                              {480, 480}, {480, 800}, {800, 480}};
  const uint32_t buttons[] = {
      H2_FRUIT_SLICE_BUTTON_START, H2_FRUIT_SLICE_BUTTON_TASKS,
      H2_FRUIT_SLICE_BUTTON_CARDS, H2_FRUIT_SLICE_BUTTON_SHOP,
      H2_FRUIT_SLICE_BUTTON_SFX,   H2_FRUIT_SLICE_BUTTON_MUSIC};
  for (unsigned size = 0; size < sizeof(sizes) / sizeof(sizes[0]); ++size) {
    h2_fruit_slice_game_init(&game, sizes[size][0], sizes[size][1], 51u,
                             UINT32_MAX);
    h2_fruit_slice_rect_t rects[6];
    for (unsigned i = 0; i < 6u; ++i) {
      CHECK(h2_fruit_slice_game_button_rect(&game, buttons[i], &rects[i]));
      CHECK(rects[i].x0 >= 8.0f && rects[i].y0 >= 8.0f);
      CHECK(rects[i].x1 <= (float)game.width - 8.0f);
      CHECK(rects[i].y1 <= (float)game.height - 8.0f);
      CHECK(rects[i].y1 - rects[i].y0 >= (i < 4u ? 32.0f : 28.0f));
      for (unsigned j = 0; j < i; ++j)
        CHECK(!rects_overlap(rects[i], rects[j]));
    }
    /* The visible fruit never sits behind a menu button. */
    const h2_fruit_slice_fruit_t* fruit = &game.fruits[0];
    const h2_fruit_slice_rect_t fruit_rect = {
        fruit->body.x - fruit->radius, fruit->body.y - fruit->radius,
        fruit->body.x + fruit->radius, fruit->body.y + fruit->radius};
    for (unsigned i = 0; i < 6u; ++i)
      CHECK(!rects_overlap(fruit_rect, rects[i]));
    for (unsigned i = 1; i <= 3u; ++i) {
      tap_button(&game, buttons[i]);
      CHECK(game.phase == (i == 1u   ? H2_FRUIT_SLICE_PHASE_TASKS
                           : i == 2u ? H2_FRUIT_SLICE_PHASE_CARDS
                                     : H2_FRUIT_SLICE_PHASE_SHOP));
      tap_button(&game, H2_FRUIT_SLICE_BUTTON_BACK);
      CHECK(game.phase == H2_FRUIT_SLICE_PHASE_TITLE);
    }
    const float x = (rects[0].x0 + rects[0].x1) * 0.5f;
    const float y = (rects[0].y0 + rects[0].y1) * 0.5f;
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_CANCEL, x, y);
    CHECK(game.phase == H2_FRUIT_SLICE_PHASE_TITLE);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, 0, 0);
    CHECK(game.phase == H2_FRUIT_SLICE_PHASE_TITLE);
    tap_button(&game, H2_FRUIT_SLICE_BUTTON_SFX);
    CHECK(game.audio_mute == H2_FRUIT_SLICE_MUTE_SFX);
    tap_button(&game, H2_FRUIT_SLICE_BUTTON_START);
    CHECK(game.phase == H2_FRUIT_SLICE_PHASE_PLAYING && game.score == 0u);
    /* Restart remains delayed; a pending reveal consumes the play tap. */
    game.phase = H2_FRUIT_SLICE_PHASE_GAME_OVER;
    game.phase_started_ms = game.time_ms;
    tap_button(&game, H2_FRUIT_SLICE_BUTTON_START);
    CHECK(game.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
    game.cards.reveal_queue = 1u;
    h2_fruit_slice_game_step(&game, 2000u);
    tap_button(&game, H2_FRUIT_SLICE_BUTTON_START);
    CHECK(game.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
    dismiss_cards(&game);
    h2_fruit_slice_game_step(&game, 2000u);
    tap_button(&game, H2_FRUIT_SLICE_BUTTON_START);
    CHECK(game.phase == H2_FRUIT_SLICE_PHASE_PLAYING);
  }
}

int main(void) {
  test_variety_ids_and_hits();
  test_natural_variety();
  test_menu_layout_and_tap_routes();
  test_progress_limits();
  test_tasks_and_shop();
  test_progress_format_and_input();
  test_weapon_reach();
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
