#include "h2_fruit_slice_game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #c); \
      exit(1);                                                      \
    }                                                               \
  } while (0)

static h2_fruit_slice_game_t game;

static void tap(h2_fruit_slice_game_t* g, uint32_t button) {
  h2_fruit_slice_rect_t r;
  CHECK(h2_fruit_slice_game_button_rect(g, button, &r));
  const float x = (r.x0 + r.x1) * 0.5f, y = (r.y0 + r.y1) * 0.5f;
  h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
  h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_UP, x, y);
}

static void visit(h2_fruit_slice_game_t* g, uint32_t button) {
  h2_fruit_slice_rect_t r;
  /* Pages never wrap; use visible navigation and require the target exists. */
  for (unsigned i = 0; i < H2_FRUIT_SLICE_TASKS; ++i) {
    if (h2_fruit_slice_game_button_rect(g, button, &r)) return;
    if (!h2_fruit_slice_game_button_rect(g, H2_FRUIT_SLICE_BUTTON_NEXT, &r))
      break;
    tap(g, H2_FRUIT_SLICE_BUTTON_NEXT);
  }
  CHECK(false);
}

static void stroke(h2_fruit_slice_game_t* g, float x0, float y0, float x1,
                   float y1) {
  h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_DOWN, x0, y0);
  h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_MOVE, x1, y1);
  h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_UP, x1, y1);
}

static void scene(h2_fruit_slice_game_t* g) {
  memset(g->fruits, 0, sizeof(g->fruits));
  g->gravity = 0.0f;
  g->pending_spawns = 0u;
  g->next_wave_ms = UINT64_MAX;
}

static void place(h2_fruit_slice_game_t* g, unsigned slot, unsigned kind,
                  float x, float y) {
  g->fruits[slot] = (h2_fruit_slice_fruit_t){
      .active = true,
      .kind = (uint8_t)kind,
      .radius = h2_fruit_slice_game_radius(g, (h2_fruit_slice_kind_t)kind),
      .body = {.x = x, .y = y}};
}

static void start(h2_fruit_slice_game_t* g) {
  h2_fruit_slice_game_init(g, 480, 480, 44u, 0u);
  stroke(g, 50, 288, 430, 288);
  CHECK(g->phase == H2_FRUIT_SLICE_PHASE_PLAYING &&
        g->progress.fruit_credit == 0u);
  scene(g);
}

static void cut(unsigned kind) {
  scene(&game);
  place(&game, 0, kind, 240, 180);
  stroke(&game, 80, 180, 400, 180);
}

static void combo(unsigned count) {
  scene(&game);
  for (unsigned i = 0; i < count; ++i)
    place(&game, i, H2_FRUIT_SLICE_APPLE, 60.0f + 80.0f * (float)i, 180);
  stroke(&game, 10, 180, 470, 180);
}

static void fall(unsigned kind) {
  scene(&game);
  place(&game, 0, kind, 240, 580);
  game.fruits[0].body.vy = 1.0f;
  h2_fruit_slice_game_step(&game, 1u);
}

static void finish_run(void) {
  cut(H2_FRUIT_SLICE_BOMB);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CARD_COUNT + 1u; ++i) {
    h2_fruit_slice_game_step(&game, 1500u);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, 240, 240);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, 240, 240);
    if (game.cards.reveal_queue == 0u && !game.cards.reveal_active) break;
  }
  CHECK(game.cards.reveal_queue == 0u && !game.cards.reveal_active);
  h2_fruit_slice_game_step(&game, H2_FRUIT_SLICE_RESTART_DELAY_MS);
}

static void test_challenge_events_and_claims(void) {
  start(&game);
  for (unsigned i = 0; i < 12u; ++i) cut(H2_FRUIT_SLICE_APPLE);
  CHECK(game.progress.challenge_credit[0] ==
        0u); /* Repeating one kind is not variety. */
  CHECK(game.progress.challenge_credit[1] ==
        0u); /* Slicing does not evade bombs. */
  CHECK(game.progress.challenge_credit[2] == 1u);
  CHECK(game.progress.challenge_credit[3] ==
        0u); /* Separate strokes are not a combo. */
  for (unsigned i = 0; i < 12u; ++i) cut(H2_FRUIT_SLICE_APPLE);
  CHECK(game.progress.challenge_credit[2] == 1u); /* One completion per run. */
  fall(H2_FRUIT_SLICE_ORANGE);
  CHECK(game.run_clean_slices == 0u && game.lives == 2u);
  cut(H2_FRUIT_SLICE_ORANGE);
  cut(H2_FRUIT_SLICE_KIWI);
  cut(H2_FRUIT_SLICE_LEMON);
  CHECK(game.progress.challenge_credit[0] == 1u);
  CHECK(game.run_clean_slices == 3u);
  const uint32_t score = game.score;
  for (unsigned i = 0; i < 3u; ++i) fall(H2_FRUIT_SLICE_BOMB);
  CHECK(game.progress.challenge_credit[1] == 1u && game.score == score &&
        game.lives == 2u);
  combo(4u);
  CHECK(game.progress.challenge_credit[3] == 1u);
  combo(4u);
  CHECK(game.progress.challenge_credit[3] == 1u);
  finish_run();
  tap(&game, H2_FRUIT_SLICE_BUTTON_TASKS);
  uint32_t expected = 0u;
  for (unsigned id = H2_FRUIT_SLICE_TASK_VARIETY; id < H2_FRUIT_SLICE_TASKS;
       ++id) {
    visit(&game, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
    expected += h2_fruit_slice_task(id)->reward;
    tap(&game, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
    CHECK(game.progress.coins == expected &&
          game.progress.challenge_credit[id - 3u] == 0u);
    tap(&game, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
    CHECK(game.progress.coins == expected);
  }
  CHECK(expected == 105u);
  uint8_t saved[H2_FRUIT_SLICE_PROGRESS_BYTES];
  h2_fruit_slice_progress_encode(&game.progress, saved);
  h2_fruit_slice_progress_t restored = {0};
  CHECK(h2_fruit_slice_progress_decode(&restored, saved, sizeof(saved)));
  CHECK(restored.coins == 105u && restored.owned == 1u &&
        restored.equipped == 0u);
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CHALLENGES; ++i)
    CHECK(restored.challenge_credit[i] == 0u);
}

static void test_run_resets_and_required_weapon(void) {
  start(&game);
  cut(H2_FRUIT_SLICE_APPLE);
  cut(H2_FRUIT_SLICE_ORANGE);
  for (unsigned i = 0; i < 6u; ++i) cut(H2_FRUIT_SLICE_APPLE);
  fall(H2_FRUIT_SLICE_LEMON);
  for (unsigned i = 0; i < 6u; ++i) cut(H2_FRUIT_SLICE_APPLE);
  CHECK(game.progress.challenge_credit[2] == 0u && game.run_clean_slices == 6u);
  fall(H2_FRUIT_SLICE_BOMB);
  fall(H2_FRUIT_SLICE_BOMB);
  finish_run();
  stroke(&game, 50, 346, 430, 346);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_PLAYING);
  CHECK(game.run_kinds == 0u && game.run_clean_slices == 0u &&
        game.run_bombs_passed == 0u);
  cut(H2_FRUIT_SLICE_KIWI);
  cut(H2_FRUIT_SLICE_LEMON);
  fall(H2_FRUIT_SLICE_BOMB);
  CHECK(game.progress.challenge_credit[0] == 0u &&
        game.progress.challenge_credit[1] == 0u);
  cut(H2_FRUIT_SLICE_APPLE);
  cut(H2_FRUIT_SLICE_ORANGE);
  CHECK(game.progress.challenge_credit[0] ==
        1u); /* Can earn again in a fresh run. */

  /* Earn/purchase through title/shop, then prove only CLASSIC qualifies. */
  h2_fruit_slice_game_init(&game, 480, 480, 45u, 0u);
  game.progress.coins = 40u;
  tap(&game, H2_FRUIT_SLICE_BUTTON_SHOP);
  tap(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + H2_FRUIT_SLICE_WEAPON_JADE);
  CHECK(game.progress.coins == 0u &&
        game.progress.equipped == H2_FRUIT_SLICE_WEAPON_JADE);
  tap(&game, H2_FRUIT_SLICE_BUTTON_BACK);
  stroke(&game, 50, 288, 430, 288);
  combo(4u);
  CHECK(game.progress.combo_credit == 1u &&
        game.progress.challenge_credit[3] == 0u);
}

static bool overlap(h2_fruit_slice_rect_t a, h2_fruit_slice_rect_t b) {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

static void test_pages_and_all_purchases(void) {
  const int sizes[][2] = {{160, 160}, {240, 240}, {480, 320}, {480, 480}};
  for (unsigned size = 0; size < 4u; ++size) {
    h2_fruit_slice_game_init(&game, sizes[size][0], sizes[size][1], 46u, 0u);
    game.progress.coins = 1000u;
    tap(&game, H2_FRUIT_SLICE_BUTTON_SHOP);
    uint32_t visited = 0u, price = 0u;
    for (;;) {
      h2_fruit_slice_progress_layout_t layout;
      h2_fruit_slice_game_progress_layout(&game, &layout);
      h2_fruit_slice_rect_t back, next;
      CHECK(h2_fruit_slice_game_button_rect(&game, H2_FRUIT_SLICE_BUTTON_BACK,
                                            &back));
      for (unsigned row = 0; row < layout.count; ++row) {
        const unsigned id = layout.first + row;
        const uint32_t button = H2_FRUIT_SLICE_BUTTON_WEAPON + id;
        h2_fruit_slice_rect_t r;
        CHECK(h2_fruit_slice_game_button_rect(&game, button, &r));
        CHECK(memcmp(&r, &layout.actions[row], sizeof(r)) == 0);
        CHECK(r.y1 <= layout.hint.y0 && layout.hint.y1 < back.y0);
        CHECK(!overlap(r, back) && !overlap(r, layout.rows[row]));
        CHECK(r.x0 >= 0 && r.x1 <= game.width && r.y0 > layout.pager.y1 &&
              r.y1 <= game.height);
        price += h2_fruit_slice_weapon(id)->price;
        tap(&game, button);
        CHECK(game.progress.equipped == id &&
              game.progress.coins == 1000u - price);
        tap(&game, button);
        CHECK(game.progress.coins == 1000u - price);
        visited |= 1u << id;
      }
      for (unsigned id = 0; id < H2_FRUIT_SLICE_WEAPONS; ++id) {
        h2_fruit_slice_rect_t r;
        CHECK(h2_fruit_slice_game_button_rect(
                  &game, H2_FRUIT_SLICE_BUTTON_WEAPON + id, &r) ==
              (id >= layout.first && id < layout.first + layout.count));
      }
      if (!h2_fruit_slice_game_button_rect(&game, H2_FRUIT_SLICE_BUTTON_NEXT,
                                           &next))
        break;
      CHECK(!overlap(next, back));
      tap(&game, H2_FRUIT_SLICE_BUTTON_NEXT);
    }
    CHECK(visited == 63u && game.progress.owned == 63u &&
          game.progress.coins == 620u);
    /* Last page is fully reachable; backward navigation is bounded too. */
    h2_fruit_slice_rect_t previous;
    while (h2_fruit_slice_game_button_rect(
        &game, H2_FRUIT_SLICE_BUTTON_PREVIOUS, &previous))
      tap(&game, H2_FRUIT_SLICE_BUTTON_PREVIOUS);
    CHECK(game.progress_page == 0u);
    tap(&game, H2_FRUIT_SLICE_BUTTON_BACK);
    tap(&game, H2_FRUIT_SLICE_BUTTON_TASKS);
    visited = 0u;
    for (unsigned id = 0; id < H2_FRUIT_SLICE_TASKS; ++id) {
      visit(&game, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
      visited |= 1u << id;
    }
    CHECK(visited == 127u);
  }
}

static bool hit_size(unsigned weapon, bool vertical, float offset,
                     float distance, unsigned kind, int32_t size) {
  h2_fruit_slice_game_init(&game, size, size, 44u, 0u);
  const float u = game.unit;
  stroke(&game, 50.0f * u, 288.0f * u, 430.0f * u, 288.0f * u);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_PLAYING);
  scene(&game);
  game.progress.owned = 63u;
  game.progress.equipped = weapon;
  place(&game, 0, kind, 240.0f * u, 180.0f * u);
  if (vertical)
    stroke(&game, (240 + offset) * u, (180 - distance * 0.5f) * u,
           (240 + offset) * u, (180 + distance * 0.5f) * u);
  else
    stroke(&game, (240 - distance * 0.5f) * u, (180 + offset) * u,
           (240 + distance * 0.5f) * u, (180 + offset) * u);
  return !game.fruits[0].active;
}

static bool hit(unsigned weapon, bool vertical, float offset, float distance,
                unsigned kind) {
  return hit_size(weapon, vertical, offset, distance, kind, 480);
}

static void test_weapon_tradeoffs_and_cards(void) {
  for (int32_t size = 160; size <= 240; size += 80) {
    CHECK(hit_size(H2_FRUIT_SLICE_WEAPON_CLASSIC, false, 27, 200,
                   H2_FRUIT_SLICE_APPLE, size));
    CHECK(!hit_size(H2_FRUIT_SLICE_WEAPON_BAMBOO, false, 27, 200,
                    H2_FRUIT_SLICE_APPLE, size));
    CHECK(hit_size(H2_FRUIT_SLICE_WEAPON_FROST, false, 40, 200,
                   H2_FRUIT_SLICE_APPLE, size));
    CHECK(!hit_size(H2_FRUIT_SLICE_WEAPON_FROST, false, 0, 8,
                    H2_FRUIT_SLICE_APPLE, size));
    CHECK(hit_size(H2_FRUIT_SLICE_WEAPON_STORM, true, 40, 200,
                   H2_FRUIT_SLICE_APPLE, size));
    CHECK(!hit_size(H2_FRUIT_SLICE_WEAPON_STORM, false, 40, 200,
                    H2_FRUIT_SLICE_APPLE, size));
    CHECK(hit_size(H2_FRUIT_SLICE_WEAPON_STORM, true, 46, 200,
                   H2_FRUIT_SLICE_BOMB, size));
  }
  CHECK(hit(H2_FRUIT_SLICE_WEAPON_CLASSIC, false, 27.0f, 200,
            H2_FRUIT_SLICE_APPLE));
  CHECK(!hit(H2_FRUIT_SLICE_WEAPON_BAMBOO, false, 27.0f, 200,
             H2_FRUIT_SLICE_APPLE));
  CHECK(!hit(H2_FRUIT_SLICE_WEAPON_CLASSIC, false, 40.0f, 200,
             H2_FRUIT_SLICE_APPLE));
  CHECK(hit(H2_FRUIT_SLICE_WEAPON_FROST, false, 40.0f, 200,
            H2_FRUIT_SLICE_APPLE));
  CHECK(!hit(H2_FRUIT_SLICE_WEAPON_FROST, false, 0, 8, H2_FRUIT_SLICE_APPLE));
  CHECK(hit(H2_FRUIT_SLICE_WEAPON_CLASSIC, false, 0, 8, H2_FRUIT_SLICE_APPLE));
  CHECK(
      !hit(H2_FRUIT_SLICE_WEAPON_STORM, false, 40, 200, H2_FRUIT_SLICE_APPLE));
  CHECK(hit(H2_FRUIT_SLICE_WEAPON_STORM, true, 40, 200, H2_FRUIT_SLICE_APPLE));
  CHECK(hit(H2_FRUIT_SLICE_WEAPON_STORM, true, 46, 200, H2_FRUIT_SLICE_BOMB));
  CHECK(!hit(H2_FRUIT_SLICE_WEAPON_STORM, false, 46, 200, H2_FRUIT_SLICE_BOMB));
  for (unsigned id = 0; id < H2_FRUIT_SLICE_WEAPONS; ++id) {
    start(&game);
    game.progress.owned = 63u;
    game.progress.equipped = id;
    scene(&game);
    for (unsigned k = 0; k < 3u; ++k)
      place(&game, k, H2_FRUIT_SLICE_APPLE, 100 + 140 * (float)k, 180);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, 40, 180);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_MOVE, 440, 180);
    h2_fruit_slice_game_step(&game, 190u);
    CHECK(game.stroke_slices == (id == H2_FRUIT_SLICE_WEAPON_STORM ? 0u : 3u));
    h2_fruit_slice_game_step(&game, 50u);
    CHECK(game.stroke_slices == (id == H2_FRUIT_SLICE_WEAPON_STORM ||
                                         id == H2_FRUIT_SLICE_WEAPON_FROST
                                     ? 0u
                                     : 3u));
    h2_fruit_slice_game_step(&game, 110u);
    CHECK(game.stroke_slices == (id == H2_FRUIT_SLICE_WEAPON_BAMBOO ? 3u : 0u));
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, 440, 180);
  }
  for (unsigned id = 0u; id < 2u; ++id) {
    start(&game);
    game.progress.equipped =
        id == 0u ? H2_FRUIT_SLICE_WEAPON_CLASSIC : H2_FRUIT_SLICE_WEAPON_BAMBOO;
    place(&game, 0u, H2_FRUIT_SLICE_APPLE, 100, 180);
    place(&game, 1u, H2_FRUIT_SLICE_APPLE, 200, 180);
    place(&game, 2u, H2_FRUIT_SLICE_APPLE, 380, 180);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, 20, 180);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_MOVE, 250, 180);
    h2_fruit_slice_game_step(&game, 350u);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_MOVE, 440, 180);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, 440, 180);
    CHECK(game.score == (id == 0u ? 3u : 6u));
  }
  for (unsigned id = 3u; id < H2_FRUIT_SLICE_WEAPONS; ++id) {
    start(&game);
    game.progress.equipped = id;
    game.progress.owned = 63u;
    for (unsigned n = 0; n < 12u; ++n) cut(H2_FRUIT_SLICE_ORANGE);
    for (unsigned card = H2_FRUIT_SLICE_CARD_CLASSIC_KNIFE;
         card <= H2_FRUIT_SLICE_CARD_FLAME_AXE; ++card)
      CHECK(game.cards.counts[card] ==
            0u); /* New weapons never index old cards. */
  }
}

static void test_exact_funds_and_claim_limits(void) {
  for (unsigned id = 1u; id < H2_FRUIT_SLICE_WEAPONS; ++id) {
    h2_fruit_slice_game_init(&game, 160, 160, 51u, 0u);
    game.progress.coins = h2_fruit_slice_weapon(id)->price - 1u;
    tap(&game, H2_FRUIT_SLICE_BUTTON_SHOP);
    visit(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + id);
    tap(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + id);
    CHECK(game.progress.owned == 1u && game.progress.equipped == 0u);
    CHECK(game.progress.coins == h2_fruit_slice_weapon(id)->price - 1u);
    ++game.progress.coins;
    tap(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + id);
    CHECK(game.progress.coins == 0u &&
          game.progress.owned == (1u | (1u << id)) &&
          game.progress.equipped == id);
  }
  for (unsigned id = 3u; id < H2_FRUIT_SLICE_TASKS; ++id) {
    h2_fruit_slice_game_init(&game, 160, 160, 52u, 0u);
    game.progress.challenge_credit[id - 3u] = 2u;
    game.progress.coins = H2_FRUIT_SLICE_PROGRESS_LIMIT;
    tap(&game, H2_FRUIT_SLICE_BUTTON_TASKS);
    visit(&game, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
    tap(&game, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
    CHECK(game.progress.coins == H2_FRUIT_SLICE_PROGRESS_LIMIT &&
          game.progress.challenge_credit[id - 3u] == 2u);
    game.progress.coins =
        H2_FRUIT_SLICE_PROGRESS_LIMIT - h2_fruit_slice_task(id)->reward;
    tap(&game, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
    CHECK(game.progress.coins == H2_FRUIT_SLICE_PROGRESS_LIMIT &&
          game.progress.challenge_credit[id - 3u] == 1u);
    game.progress.coins = 0u;
    tap(&game, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
    CHECK(game.progress.coins == h2_fruit_slice_task(id)->reward &&
          game.progress.challenge_credit[id - 3u] == 0u);
    tap(&game, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
    CHECK(game.progress.coins == h2_fruit_slice_task(id)->reward);
  }
  h2_fruit_slice_game_init(&game, 160, 160, 53u, 0u);
  tap(&game, H2_FRUIT_SLICE_BUTTON_SHOP);
  h2_fruit_slice_rect_t next;
  CHECK(h2_fruit_slice_game_button_rect(&game, H2_FRUIT_SLICE_BUTTON_NEXT,
                                        &next));
  const float x = (next.x0 + next.x1) * 0.5f, y = (next.y0 + next.y1) * 0.5f;
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_CANCEL, x, y);
  CHECK(game.progress_page == 0u && game.progress.coins == 0u &&
        !game.progress_dirty);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, 0, 0);
  CHECK(game.progress_page == 0u && game.progress.coins == 0u);
}

static void test_legacy_and_invalid_formats(void) {
  /* Original 0.2.0 format bytes; not generated by the new serializer. */
  const uint8_t legacy[32] = {0x46, 0x53, 0x50, 0x31, 1, 0, 0, 0, 123, 0, 0,
                              0,    9,    0,    0,    0, 2, 0, 0, 0,   2, 0,
                              0,    0,    7,    0,    0, 0, 2, 0, 0,   0};
  h2_fruit_slice_progress_t p = {0};
  CHECK(h2_fruit_slice_progress_decode(&p, legacy, sizeof(legacy)));
  CHECK(p.coins == 123u && p.fruit_credit == 9u && p.combo_credit == 2u &&
        p.score_task == 2u && p.owned == 7u && p.equipped == 2u);
  for (unsigned i = 0; i < 4u; ++i) CHECK(p.challenge_credit[i] == 0u);
  p.owned = 63u;
  p.equipped = 5u;
  p.challenge_credit[3] = 3u;
  uint8_t bytes[H2_FRUIT_SLICE_PROGRESS_BYTES];
  h2_fruit_slice_progress_encode(&p, bytes);
  CHECK(bytes[4] == 2u);
  h2_fruit_slice_progress_t restored = {0};
  CHECK(h2_fruit_slice_progress_decode(&restored, bytes, sizeof(bytes)));
  CHECK(memcmp(&restored, &p, sizeof(p)) == 0);
  for (unsigned scenario = 0; scenario < 5u; ++scenario) {
    uint8_t bad[H2_FRUIT_SLICE_PROGRESS_BYTES];
    memcpy(bad, bytes, sizeof(bad));
    size_t size = sizeof(bad);
    if (scenario == 0u) bad[4] = 3u;
    if (scenario == 1u) bad[24] = 127u;
    if (scenario == 2u) bad[24] = 1u;
    if (scenario == 3u) memset(bad + 32, 255, 4u);
    if (scenario == 4u) --size;
    CHECK(!h2_fruit_slice_progress_decode(&restored, bad, size));
    CHECK(memcmp(&restored, &p, sizeof(p)) == 0);
  }
  uint8_t bad_legacy[32];
  memcpy(bad_legacy, legacy, sizeof(legacy));
  bad_legacy[24] = 63u;
  bad_legacy[28] = 5u;
  CHECK(!h2_fruit_slice_progress_decode(&restored, bad_legacy,
                                        sizeof(bad_legacy)));
}

int main(void) {
  test_challenge_events_and_claims();
  test_run_resets_and_required_weapon();
  test_pages_and_all_purchases();
  test_weapon_tradeoffs_and_cards();
  test_exact_funds_and_claim_limits();
  test_legacy_and_invalid_formats();
  puts("progression 0.3 tests passed");
  return 0;
}
