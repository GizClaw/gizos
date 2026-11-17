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
  CHECK(h2_fruit_slice_cards_merge(&restored, data, sizeof(data)));
  CHECK(restored.counts[H2_FRUIT_SLICE_CARD_BOMB] == 2u);
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

int main(void) {
  test_catalog_and_awards();
  test_storage();
  test_game_routes_and_reveal();
  puts("fruit card tests passed");
  return 0;
}
