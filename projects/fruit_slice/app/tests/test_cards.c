#include "h2_fruit_slice_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); \
      exit(1);                                                \
    }                                                         \
  } while (0)

static void test_catalog_and_awards(void) {
  h2_fruit_slice_cards_t c;
  h2_fruit_slice_cards_init(&c, 240, 280, 1u);
  unsigned total = 0;
  for (unsigned tab = 0; tab < H2_FRUIT_SLICE_CARD_TABS; ++tab) {
    uint8_t ids[H2_FRUIT_SLICE_CARD_TAB_SLOTS];
    const unsigned n = h2_fruit_slice_card_tab_cards(tab, ids);
    CHECK(n > 0u && n <= H2_FRUIT_SLICE_CARD_TAB_SLOTS);
    total += n;
    for (unsigned i = 0; i < n; ++i)
      CHECK(h2_fruit_slice_card(ids[i])->category == tab);
  }
  CHECK(total == 16u);
  h2_fruit_slice_cards_on_bomb_hit(&c, 20, 20);
  CHECK(c.run_earned == 0u);
  h2_fruit_slice_cards_run_start(&c);
  for (unsigned kind = 0; kind < 5u; ++kind)
    for (unsigned n = 0; n < H2_FRUIT_SLICE_CARD_DROP_PITY; ++n)
      h2_fruit_slice_cards_on_slice(&c, kind, 60u, 0u, 120, 140);
  h2_fruit_slice_cards_on_slice(&c, 0u, 60u, 1u, 120, 140);
  h2_fruit_slice_cards_on_slice(&c, 0u, 60u, 2u, 120, 140);
  h2_fruit_slice_cards_on_combo(&c, 5u, 60u, 2u, 120, 140);
  for (unsigned n = 0; n < 8u; ++n)
    h2_fruit_slice_cards_on_bomb_passed(&c, 120, 280);
  h2_fruit_slice_cards_on_bomb_hit(&c, 120, 140);
  CHECK(h2_fruit_slice_cards_owned_total(&c) == 16u);
  for (unsigned i = 0; i < 16u; ++i) CHECK(c.counts[i] == 1u);
  CHECK(c.cue_count <= H2_FRUIT_SLICE_CARD_CUES);
  CHECK(h2_fruit_slice_cards_run_landed(&c) == 12u);
  h2_fruit_slice_cards_step(&c, 1000u, false);
  CHECK(h2_fruit_slice_cards_run_landed(&c) == 16u);
  h2_fruit_slice_cards_run_end(&c);
  h2_fruit_slice_cards_run_start(&c);
  h2_fruit_slice_cards_on_bomb_hit(&c, 120, 140);
  h2_fruit_slice_cards_on_bomb_hit(&c, 120, 140);
  CHECK(c.counts[H2_FRUIT_SLICE_CARD_BOMB] == 2u);
  CHECK(c.run_new == 0u);
  c.counts[H2_FRUIT_SLICE_CARD_BOMB] = 99u;
  h2_fruit_slice_cards_run_end(&c);
  h2_fruit_slice_cards_run_start(&c);
  h2_fruit_slice_cards_on_bomb_hit(&c, 120, 140);
  CHECK(c.counts[H2_FRUIT_SLICE_CARD_BOMB] == 99u);
}

static void test_storage(void) {
  h2_fruit_slice_cards_t c, restored;
  h2_fruit_slice_cards_init(&c, 240, 280, 1u);
  h2_fruit_slice_cards_run_start(&c);
  h2_fruit_slice_cards_on_bomb_hit(&c, 10, 10);
  uint8_t data[H2_FRUIT_SLICE_CARDS_BYTES], bad[H2_FRUIT_SLICE_CARDS_BYTES];
  h2_fruit_slice_cards_encode(&c, data);
  h2_fruit_slice_cards_init(&restored, 240, 280, 2u);
  CHECK(h2_fruit_slice_cards_decode(&restored, data, sizeof(data)));
  CHECK(restored.counts[H2_FRUIT_SLICE_CARD_BOMB] == 1u);
  CHECK(restored.unseen == (1u << H2_FRUIT_SLICE_CARD_BOMB));
  const h2_fruit_slice_cards_t before = restored;
  for (unsigned i = 0; i < 5u; ++i) {
    memcpy(bad, data, sizeof(data));
    if (i == 0u) bad[0] ^= 1u;
    if (i == 1u) bad[4] = 2u;
    if (i == 2u) bad[12] = 17u;
    if (i == 3u) bad[16] = 100u;
    if (i == 4u) bad[8] = 1u;
    CHECK(!h2_fruit_slice_cards_decode(&restored, bad, sizeof(bad)));
    CHECK(memcmp(&before, &restored, sizeof(before)) == 0);
  }
  CHECK(!h2_fruit_slice_cards_decode(&restored, data, sizeof(data) - 1u));
  CHECK(!h2_fruit_slice_cards_decode(&restored, NULL, sizeof(data)));
  h2_fruit_slice_cards_init(&restored, 240, 280, 2u);
  h2_fruit_slice_cards_run_start(&restored);
  h2_fruit_slice_cards_on_bomb_hit(&restored, 10, 10);
  CHECK(h2_fruit_slice_cards_merge(&restored, data, sizeof(data)));
  CHECK(restored.counts[H2_FRUIT_SLICE_CARD_BOMB] == 2u);
  const h2_fruit_slice_cards_t merged = restored;
  CHECK(!h2_fruit_slice_cards_merge(&restored, data, sizeof(data)));
  CHECK(memcmp(&merged, &restored, sizeof(merged)) == 0);
}

static void tap(h2_fruit_slice_game_t* g, uint32_t button) {
  h2_fruit_slice_rect_t r;
  CHECK(h2_fruit_slice_game_button_rect(g, button, &r));
  const float x = (r.x0 + r.x1) * 0.5f, y = (r.y0 + r.y1) * 0.5f;
  h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
  h2_fruit_slice_game_pointer(g, H2_FRUIT_SLICE_POINTER_UP, x, y);
}

static void test_game_routes_and_reveal(void) {
  h2_fruit_slice_game_t g;
  h2_fruit_slice_game_init(&g, 240, 280, 7u, 0u);
  tap(&g, H2_FRUIT_SLICE_BUTTON_CARDS);
  CHECK(g.phase == H2_FRUIT_SLICE_PHASE_CARDS && g.cards.album_open);
  h2_fruit_slice_album_layout_t l;
  h2_fruit_slice_cards_album_layout(g.width, g.height, g.unit, &l);
  const float x = (l.tabs[3].x0 + l.tabs[3].x1) * 0.5f;
  const float y = (l.tabs[3].y0 + l.tabs[3].y1) * 0.5f;
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_UP, x, y);
  CHECK(g.cards.tab == H2_FRUIT_SLICE_CARD_TAB_SKILL);
  h2_fruit_slice_game_step(&g, 400u);
  CHECK(!h2_fruit_slice_cards_tab_sliding(&g.cards));
  tap(&g, H2_FRUIT_SLICE_BUTTON_BACK);
  CHECK(g.phase == H2_FRUIT_SLICE_PHASE_TITLE && !g.cards.album_open);
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_DOWN, 0, 168);
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_UP, 240, 168);
  CHECK(g.phase == H2_FRUIT_SLICE_PHASE_PLAYING && g.cards.run_active);
  CHECK(g.cards.run_earned == 0u); /* Menu fruit earns nothing. */
  memset(g.fruits, 0, sizeof(g.fruits));
  g.fruits[0] = (h2_fruit_slice_fruit_t){.active = true,
                                         .kind = H2_FRUIT_SLICE_BOMB,
                                         .radius = 15,
                                         .body = {.x = 120, .y = 140}};
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_DOWN, 0, 140);
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_UP, 240, 140);
  CHECK(g.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
  CHECK(g.cards.counts[H2_FRUIT_SLICE_CARD_BOMB] == 1u);
  h2_fruit_slice_game_step(&g, 720u);
  CHECK(g.cards.reveal_active);
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_UP, 120, 140);
  CHECK(g.cards.reveal_ms < H2_FRUIT_SLICE_REVEAL_FACE_MS);
  h2_fruit_slice_game_step(&g, 1200u);
  CHECK(g.cards.reveal_cued);
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_DOWN, 120, 140);
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_UP, 120, 140);
  CHECK(!g.cards.reveal_active && g.cards.reveal_queue == 0u);
  CHECK(g.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
  h2_fruit_slice_game_step(&g, 1000u);
  tap(&g, H2_FRUIT_SLICE_BUTTON_CARDS);
  CHECK(g.cards.tab == H2_FRUIT_SLICE_CARD_TAB_BOMB);
  CHECK(g.cards.unseen == 0u);
  tap(&g, H2_FRUIT_SLICE_BUTTON_BACK);
  CHECK(g.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
  bool heard_card = false;
  h2_fruit_slice_sound_t sound;
  while (h2_fruit_slice_game_take_sound(&g, &sound))
    heard_card |= sound == H2_FRUIT_SLICE_SOUND_CARD;
  CHECK(heard_card);
}

static void test_play_awards_and_rare_sound(void) {
  h2_fruit_slice_game_t g;
  h2_fruit_slice_game_init(&g, 240, 240, 4u, 0u);
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_DOWN, 0, 144);
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_UP, 240, 144);
  g.next_wave_ms = UINT64_MAX;
  for (unsigned run = 0; run < 20u; ++run) {
    memset(g.fruits, 0, sizeof(g.fruits));
    for (unsigned kind = 0; kind < 5u; ++kind)
      g.fruits[kind] = (h2_fruit_slice_fruit_t){
          .active = true,
          .kind = (uint8_t)kind,
          .radius = 10,
          .body = {.x = 30.0f + 40.0f * (float)kind, .y = 120}};
    h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_DOWN, 0, 120);
    h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_UP, 240, 120);
  }
  CHECK(g.score == 200u && g.sliced_total == 100u);
  CHECK(g.cards.counts[H2_FRUIT_SLICE_CARD_COMBO] == 1u);
  CHECK(g.cards.counts[H2_FRUIT_SLICE_CARD_MEGA_COMBO] == 1u);
  CHECK(g.cards.counts[H2_FRUIT_SLICE_CARD_CLASSIC_KNIFE] == 1u);
  CHECK(g.cards.counts[H2_FRUIT_SLICE_CARD_HIGH_SCORE] == 1u);
  CHECK(g.cards.counts[H2_FRUIT_SLICE_CARD_LEGEND] == 1u);
  for (unsigned kind = 0; kind < 5u; ++kind) CHECK(g.cards.counts[kind] == 1u);
  for (unsigned n = 0; n < 8u; ++n) {
    g.fruits[0] =
        (h2_fruit_slice_fruit_t){.active = true,
                                 .kind = H2_FRUIT_SLICE_BOMB,
                                 .radius = 15,
                                 .body = {.x = 120, .y = 260, .vy = 1}};
    h2_fruit_slice_game_step(&g, 16u);
  }
  CHECK(g.cards.counts[H2_FRUIT_SLICE_CARD_CLOSE_CALL] == 1u);
  CHECK(g.cards.counts[H2_FRUIT_SLICE_CARD_BOMB_SQUAD] == 1u);
  g.fruits[0].active = true;
  g.fruits[0].body.y = 120;
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_DOWN, 0, 120);
  h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_UP, 240, 120);
  CHECK(g.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
  bool heard_rare = false;
  for (unsigned n = 0; n < 16u && g.cards.reveal_queue; ++n) {
    h2_fruit_slice_sound_t sound;
    while (h2_fruit_slice_game_take_sound(&g, &sound)) {
    }
    h2_fruit_slice_game_step(&g, 2000u);
    while (h2_fruit_slice_game_take_sound(&g, &sound))
      heard_rare |= sound == H2_FRUIT_SLICE_SOUND_RARE;
    h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_DOWN, 120, 120);
    h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_UP, 120, 120);
  }
  CHECK(heard_rare && !g.cards.reveal_active && g.cards.reveal_queue == 0u);
}

static void test_pending_collection_merge(void) {
  h2_fruit_slice_cards_t stored, c;
  h2_fruit_slice_cards_init(&stored, 240, 240, 1u);
  stored.counts[0] = stored.counts[1] = stored.counts[2] = 5u;
  stored.counts[3] = 99u;
  stored.unseen = (1u << 0) | (1u << 2) | (1u << 3);
  uint8_t data[H2_FRUIT_SLICE_CARDS_BYTES];
  h2_fruit_slice_cards_encode(&stored, data);
  h2_fruit_slice_cards_init(&c, 240, 240, 2u);
  h2_fruit_slice_cards_run_start(&c);
  for (unsigned id = 0; id < 5u; ++id) {
    c.pity[id] = H2_FRUIT_SLICE_CARD_DROP_PITY - 1u;
    h2_fruit_slice_cards_on_slice(&c, id, 1u, 0u, 120, 120);
  }
  h2_fruit_slice_cards_on_bomb_hit(&c, 120, 120);
  h2_fruit_slice_cards_open_album(&c); /* Inspect the locally earned melon. */
  CHECK(c.seen_before_load == 1u && (c.unseen & 1u) == 0u);
  h2_fruit_slice_cards_close_album(&c);
  h2_fruit_slice_cards_run_end(&c);
  while (h2_fruit_slice_cards_take_cue(&c) != H2_FRUIT_SLICE_CARD_CUE_NONE) {
  }
  h2_fruit_slice_cards_step(&c, 1u, true);
  h2_fruit_slice_cards_step(&c, 850u, true);
  const h2_fruit_slice_cards_t before = c;
  CHECK(c.reveal_active && c.reveal_card == 0u && c.reveal_cued);
  CHECK(h2_fruit_slice_cards_merge(&c, data, sizeof(data)));
  CHECK(c.loaded && c.dirty);
  CHECK(c.counts[0] == 6u && c.counts[1] == 6u && c.counts[2] == 6u);
  CHECK(c.counts[3] == 99u && c.counts[4] == 1u);
  CHECK(c.counts[H2_FRUIT_SLICE_CARD_BOMB] == 1u);
  const uint32_t fresh = (1u << 4) | (1u << H2_FRUIT_SLICE_CARD_BOMB);
  CHECK(c.unseen == ((1u << 2) | (1u << 3) | fresh));
  CHECK(c.run_new == fresh && c.run_earned == before.run_earned);
  CHECK(c.reveal_active && c.reveal_card == before.reveal_card &&
        c.reveal_ms == before.reveal_ms && c.reveal_cued == before.reveal_cued);
  CHECK(c.reveal_queue == (fresh | 1u) && c.reveal_total == 3u);
  CHECK(c.cue_count == before.cue_count &&
        memcmp(c.cues, before.cues, sizeof(c.cues)) == 0);
  CHECK(c.rng == before.rng && c.time_ms == before.time_ms);
  CHECK(memcmp(c.sparks, before.sparks, sizeof(c.sparks)) == 0);
  const h2_fruit_slice_cards_t merged = c;
  CHECK(!h2_fruit_slice_cards_merge(&c, data, sizeof(data)));
  CHECK(memcmp(&c, &merged, sizeof(c)) == 0);
  unsigned completed = 0u;
  while (c.reveal_active) {
    h2_fruit_slice_cards_step(&c, 1200u, true);
    (void)h2_fruit_slice_cards_reveal_pointer(&c, H2_FRUIT_SLICE_POINTER_DOWN);
    (void)h2_fruit_slice_cards_reveal_pointer(&c, H2_FRUIT_SLICE_POINTER_UP);
    CHECK(++completed <= 3u);
  }
  CHECK(completed == 3u && c.reveal_queue == 0u);
  CHECK(h2_fruit_slice_cards_take_cue(&c) == H2_FRUIT_SLICE_CARD_CUE_CARD);
  CHECK(h2_fruit_slice_cards_take_cue(&c) == H2_FRUIT_SLICE_CARD_CUE_CARD);
  CHECK(h2_fruit_slice_cards_take_cue(&c) == H2_FRUIT_SLICE_CARD_CUE_RARE);
  CHECK(h2_fruit_slice_cards_take_cue(&c) == H2_FRUIT_SLICE_CARD_CUE_NONE);
}

int main(void) {
  test_pending_collection_merge();
  test_play_awards_and_rare_sound();
  test_catalog_and_awards();
  test_storage();
  test_game_routes_and_reveal();
  puts("fruit card tests passed");
  return 0;
}
