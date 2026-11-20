#include "buffet_draw.h"
#include "buffet_rotary.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      exit(1);                                                \
    }                                                         \
  } while (0)
static bf_game_t paid(void) {
  bf_game_t g;
  bf_init(&g, false);
  bf_tap(&g, 610, 383);
  CHECK(g.phase == BF_DINING);
  CHECK(g.purse == bf_rules.purse - bf_rules.ticket);
  return g;
}
static void clock_and_stomach(void) {
  bf_game_t g = paid();
  g.fullness_milli = bf_rules.capacity * 1000 + 1;
  bf_step(&g, 1, 0, 0);
  CHECK(g.red_ms == 0);
  g.fullness_milli = bf_rules.red_line * 1000;
  bf_step(&g, 100, 0, 0);
  CHECK(g.red_ms == 0);
  g.fullness_milli = bf_rules.red_line * 1000 + 2000;
  bf_step(&g, 999, 0, 0);
  CHECK(g.red_ms == 999);
  bf_step(&g, 1, 0, 0);
  CHECK(g.red_ms == 0 && g.fullness_milli == bf_rules.red_line * 1000);
  g.fullness_milli += 50000;
  bf_step(&g, bf_rules.tolerance_ms - 1, 0, 0);
  CHECK(g.phase == BF_DINING && g.red_ms == bf_rules.tolerance_ms - 1);
  int now = g.elapsed_ms;
  bf_step(&g, 400, 0, 0);
  CHECK(g.phase == BF_HOSPITAL && g.elapsed_ms == now + 1);
  CHECK(g.purse == bf_rules.purse - bf_rules.ticket - bf_rules.fee);
  bf_step(&g, 5000, 0, 0);
  CHECK(g.elapsed_ms == now + 1);
  g = paid();
  g.fullness_milli = bf_rules.red_line * 1000 +
                     bf_rules.tolerance_ms * bf_rules.digest_milli_per_ms;
  bf_step(&g, bf_rules.tolerance_ms, 0, 0);
  CHECK(g.phase == BF_DINING && g.red_ms == 0);
  g = paid();
  g.elapsed_ms = bf_rules.duration_ms - bf_rules.tolerance_ms;
  g.fullness_milli = 200000;
  bf_step(&g, 20000, 0, 0);
  CHECK(g.phase == BF_CLOSING && g.elapsed_ms == bf_rules.duration_ms &&
        g.fullness_milli > bf_rules.red_line * 1000);
  CHECK(g.purse == bf_rules.purse - bf_rules.ticket);
  bf_step(&g, 999999, 0, 0);
  CHECK(g.phase == BF_CLOSING);
  g = paid();
  g.elapsed_ms = bf_rules.duration_ms - bf_rules.tolerance_ms - 1;
  g.fullness_milli = 200000;
  bf_step(&g, 20000, 0, 0);
  CHECK(g.phase == BF_HOSPITAL && g.elapsed_ms == bf_rules.duration_ms - 1);
}
static void handheld_control_layout(void) {
  /* The board projects stage units at 3/5 scale, with a 16px band below. */
  const bf_rect_t stick = bf_stick_bounds, eat = bf_eat_bounds;
  CHECK(stick.x + stick.w / 2 == BF_STICK_X && eat.x + eat.w / 2 == BF_EAT_X &&
        BF_STICK_X == H2_BUFFET_WIDTH - BF_EAT_X);
  CHECK(stick.w == 144 && stick.h == 130 && eat.w == 104 && eat.h == eat.w);
  CHECK(stick.y == BF_CONTROL_Y - BF_STICK_HIT_UP &&
        eat.y + eat.h / 2 == BF_CONTROL_Y);
  /* Even the outermost accepted origin leaves 24 screen pixels for grip
   * after a full-speed throw. The smaller Eat surface clears the same rim. */
  CHECK((stick.x - BF_STICK_TRAVEL) * 3 / 5 >= 24);
  CHECK(16 + (H2_BUFFET_HEIGHT - stick.y - stick.h - BF_STICK_TRAVEL) * 3 / 5 >=
        24);
  CHECK((H2_BUFFET_WIDTH - eat.x - eat.w) * 3 / 5 >= 24);

  bf_game_t g = paid();
  /* A 480px-panel thumb landing at x=56 used to miss the x=61.2 start. */
  const float landing_x = 56.f * 5 / 3;
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_DOWN, 0, landing_x, BF_CONTROL_Y);
  CHECK(g.controls.stick_held && g.controls.stick.x == 0);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_MOVE, 0, landing_x - BF_STICK_TRAVEL,
             BF_CONTROL_Y);
  CHECK(g.controls.stick_held && g.controls.stick.x < -.999f);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_MOVE, 0, 0, BF_CONTROL_Y);
  CHECK(g.controls.stick_held && g.controls.stick.x < -.999f);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_MOVE, 0, landing_x, BF_CONTROL_Y);
  CHECK(g.controls.stick_held && g.controls.stick.x == 0);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_MOVE, 0, landing_x - BF_STICK_TRAVEL,
             BF_CONTROL_Y);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_UP, 0, 0, BF_CONTROL_Y);
  CHECK(!g.controls.stick_held && g.controls.stick.x == 0);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_DOWN, 0, stick.x - 1, BF_CONTROL_Y);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_MOVE, 0, BF_STICK_X, BF_CONTROL_Y);
  CHECK(!g.controls.stick_held);
}
static void play_with_controls(void) {
  bf_game_t g = paid();
  const uint32_t stick = 0, other = 17;
  bf_point_t start = g.player.pos;
  bf_place_t p = bf_place(&g, 4);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_DOWN, stick, p.pos.x, p.pos.y);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_UP, stick, p.pos.x, p.pos.y);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_DOWN, stick, BF_EAT_X, BF_CONTROL_Y);
  bf_step(&g, 33, 0, 0);
  CHECK(g.eaten == 0 && g.player.pos.x == start.x && g.player.pos.y == start.y);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_UP, stick, BF_EAT_X, BF_CONTROL_Y);

  bf_pointer(&g, H2_PAL_TOUCH_EVENT_DOWN, stick, BF_STICK_X, BF_CONTROL_Y);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_MOVE, stick, BF_STICK_X + 5, BF_CONTROL_Y);
  bf_step(&g, 33, 0, 0);
  CHECK(g.player.pos.x == start.x && !g.player.walking);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_MOVE, stick, 300, 300);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_UP, other, 300, 300);
  CHECK(g.controls.stick_held);
  float mag = hypotf(g.controls.stick.x, g.controls.stick.y);
  CHECK(fabsf(mag - 1) < .001f);
  bf_step(&g, 33, 0, 0);
  CHECK(g.player.pos.x > start.x && g.player.pos.y < start.y);
  CHECK(fabsf(hypotf(g.player.pos.x - start.x,
                     (g.player.pos.y - start.y) / .75f) -
              33 * .173f) < .01f);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_UP, stick, 300, 300);
  start = g.player.pos;
  bf_step(&g, 90, 0, 0);
  CHECK(g.player.pos.x == start.x && g.player.pos.y == start.y &&
        !g.player.walking);

  g.player.pos = bf_place(&g, 4).station;
  int target = bf_eat_target(&g);
  CHECK(target == 4);
  p = bf_place(&g, target);
  bf_tap(&g, p.pos.x, p.pos.y);
  CHECK(g.eaten == 0);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_DOWN, stick, BF_EAT_X, BF_CONTROL_Y);
  CHECK(g.eaten == 1 && g.returned == 1 && g.settings[target].dish < 0);
  CHECK(g.value == bf_menu[0].coins &&
        g.fullness_milli == bf_menu[0].fullness * 1000);
  CHECK(bf_eat_target(&g) < 0);
  bf_step(&g, 1000, 0, 0);
  g.player.pos = bf_place(&g, BF_SETTINGS + 6).station;
  CHECK(bf_eat_target(&g) == BF_SETTINGS + 6);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_DOWN, stick, BF_EAT_X, BF_CONTROL_Y);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_MOVE, stick, BF_EAT_X - 5, BF_CONTROL_Y);
  CHECK(g.eaten == 1);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_UP, stick, BF_EAT_X, BF_CONTROL_Y);

  bf_pointer(&g, H2_PAL_TOUCH_EVENT_DOWN, stick, BF_STICK_X, BF_CONTROL_Y);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_MOVE, stick, BF_STICK_X + 60, BF_CONTROL_Y);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_DOWN, other, BF_EAT_X, BF_CONTROL_Y);
  CHECK(g.eaten == 2 && g.controls.stick_held);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_CANCEL, other, BF_EAT_X, BF_CONTROL_Y);
  CHECK(g.controls.stick_held && !g.controls.press_held);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_CANCEL, H2_PAL_TOUCH_ALL_POINTERS, 0, 0);
  CHECK(!g.controls.stick_held);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_DOWN, stick, 765, 28);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_CANCEL, stick, 765, 28);
  CHECK(!g.muted);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_DOWN, stick, 765, 28);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_UP, stick, 765, 28);
  CHECK(g.muted);

  bf_step(&g, 1000, 0, 0);
  g.player.pos = bf_project((bf_world_t){6, 10.35f}, 0);
  for (int i = 0; i < 24; i++) bf_step(&g, 33, 0, -1);
  CHECK(bf_unproject(g.player.pos).v >= 10.22f && bf_walkable(g.player.pos));
  bf_point_t before = g.player.pos;
  bf_step(&g, 60, 1, 0);
  CHECK(g.player.pos.x > before.x && g.player.face == BF_RIGHT);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_DOWN, stick, BF_STICK_X, BF_CONTROL_Y);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_MOVE, stick, BF_STICK_X + 60, BF_CONTROL_Y);
  bf_step(&g, bf_rules.duration_ms, 0, 0);
  CHECK(g.phase == BF_CLOSING && !g.controls.stick_held);
  bf_step(&g, 700, 0, 0);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.phase == BF_TICKET && g.purse == bf_rules.purse);
  bf_action(&g, H2_BUFFET_LANGUAGE | H2_BUFFET_GOAL);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.en && g.tasting && g.eaten == 0 && g.elapsed_ms == 0 &&
        g.purse == bf_rules.purse - bf_rules.ticket);
}
static void inspect_cards(void) {
  bf_game_t g = paid();
  g.eaten = 1;
  bf_step(&g, bf_rules.duration_ms, 0, 0);
  bf_confirm_cards(&g.album, 3u);
  CHECK(g.album.reveal_ms == 0 && g.album.fresh == 3u && g.album.owned == 3u);
  bf_step(&g, bf_reveal_duration(&g), 0, 0);
  bf_tap(&g, 61, 383);
  CHECK(g.album.inspecting && g.album.inspect_receipt && g.album.selected == 0);
  bf_step(&g, 100, 0, 0);
  CHECK(g.album.inspect_ms == 100);
  bf_tap(&g, 705, 50);
  CHECK(!g.album.inspecting && !g.album.open);
  bf_tap(&g, 650, 29);
  CHECK(g.album.open && !g.album.this_meal);
  bf_tap(&g, bf_card_position(&g, 1), 185);
  CHECK(g.album.inspecting && !g.album.inspect_receipt &&
        g.album.selected == 1 && g.album.inspect_ms == 0);
  bf_step(&g, 500, 0, 0);
  CHECK(g.album.inspect_ms == BF_INSPECT_MS);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.album.open && !g.album.inspecting);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.album.inspecting && g.album.inspect_ms == 0);
  bf_action(&g, H2_BUFFET_ALBUM);
  g.album.reduced_motion = true;
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.album.inspecting && g.album.inspect_ms == BF_INSPECT_MS);
  CHECK(g.album.reveal_ms == bf_reveal_duration(&g) && g.album.owned == 3u &&
        g.album.fresh == 3u && !g.album.awaiting);
  bf_init(&g, false);
  bf_tap(&g, 650, 29);
  bf_tap(&g, bf_card_position(&g, 0), 185);
  CHECK(g.album.open && !g.album.inspecting && g.album.selected == 0);
}
static void fonts_and_servings(void) {
  bf_game_t g = paid();
  for (int i = 0; i < BF_DISHES; i++) {
    CHECK(bf_menu[i].id && bf_menu[i].coins > 0 && bf_menu[i].fullness > 0);
    for (int j = 0; j < i; j++)
      CHECK(strcmp(bf_menu[i].id, bf_menu[j].id) != 0);
    for (int size = 0; size < 4; size++) {
      CHECK(bf_font_covers(false, size, bf_menu[i].name.zh));
      CHECK(bf_font_covers(false, size, bf_menu[i].detail.zh));
      CHECK(bf_font_covers(true, size, bf_menu[i].name.en));
      CHECK(bf_font_covers(true, size, "中文"));
    }
    CHECK(bf_width(true, BF_HEAD, bf_menu[i].name.en) <= 430);
  }
  unsigned seen = 0;
  for (int t = 0; t < BF_TABLES; t++) {
    CHECK(bf_font_covers(false, BF_SMALL, bf_tables[t].name.zh));
    for (int i = 0; i < 4; i++) {
      g.service[t].course = i;
      int d = bf_next_dish(&g, t);
      CHECK(d >= 0 && d < BF_DISHES);
      seen |= 1u << d;
    }
  }
  for (int i = 0; i < BF_SLOTS; i++)
    CHECK(bf_walkable(bf_place(&g, i).station));
  CHECK(seen == (1u << BF_DISHES) - 1);
}
int main(void) {
  clock_and_stomach();
  handheld_control_layout();
  play_with_controls();
  inspect_cards();
  fonts_and_servings();
  puts(
      "buffet rules, touch controls, card inspection, replay, menu and fonts passed");
  return 0;
}
