#include "buffet_game.h"

#include <limits.h>
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

static bf_game_t seated(void) {
  bf_game_t g;
  bf_init(&g, true);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.phase == BF_DINING && g.meal_menu == BF_LIVE);
  return g;
}
static void contact(bf_game_t* g, h2_pal_touch_event_kind_t kind,
                    uint32_t finger, bf_rect_t b) {
  bf_pointer(g, kind, finger, b.x + b.w * .5f, b.y + b.h * .5f);
}
static void tap(bf_game_t* g, bf_rect_t b) {
  contact(g, H2_PAL_TOUCH_EVENT_DOWN, 0, b);
  contact(g, H2_PAL_TOUCH_EVENT_UP, 0, b);
}

static void freeze_the_meal(void) {
  bf_game_t g = seated();
  const int ramen = BF_FIRST_HOT + 2;
  bf_step(&g, 25999, 0, 0);
  CHECK(bf_fresh_left(&g, ramen) == 1);
  int queue[BF_FRESH_QUEUE];
  int queued = bf_expiring(&g, queue);
  CHECK(queued > 0 && queue[0] == ramen);
  g.fullness_milli = 149000;
  g.red_ms = 6500;
  g.bite_ms = 340;
  g.gauge_milli = 142000;
  g.player.stride = 9.125f;
  /* Open from a second finger while the stick owns a drag. */
  contact(&g, H2_PAL_TOUCH_EVENT_DOWN, 3, bf_stick_bounds);
  bf_pointer(&g, H2_PAL_TOUCH_EVENT_MOVE, 3, BF_STICK_X - 40, BF_CONTROL_Y);
  CHECK(g.controls.stick_held && g.controls.stick.x < 0);
  tap(&g, bf_menu_button);
  CHECK(g.meal_menu == BF_MEAL_STATUS && !g.controls.stick_held &&
        !g.controls.press_held);
  bf_game_t paused = g;
  bf_step(&g, INT_MAX, 1, -1);
  CHECK(memcmp(&g, &paused, sizeof(g)) == 0);
  CHECK(bf_eat_target(&g) < 0);
  int paused_queue[BF_FRESH_QUEUE];
  CHECK(bf_expiring(&g, paused_queue) == queued &&
        !memcmp(queue, paused_queue, (size_t)queued * sizeof(queue[0])));
  contact(&g, H2_PAL_TOUCH_EVENT_UP, 3, bf_stick_bounds);
  tap(&g, bf_eat_bounds);
  tap(&g, bf_sound_bounds(&g));
  CHECK(g.eaten == 0 && !g.muted && g.meal_menu == BF_MEAL_STATUS);
  tap(&g, bf_pause_left);
  CHECK(g.meal_menu == BF_CONFIRM_LEAVE && !g.leave_selected);
  tap(&g, bf_pause_left);
  CHECK(g.meal_menu == BF_MEAL_STATUS);
  tap(&g, bf_pause_right);
  CHECK(g.meal_menu == BF_LIVE && g.elapsed_ms == 25999 && g.bite_ms == 340 &&
        g.red_ms == 6500);
  bf_step(&g, 1, 0, 0);
  CHECK(!bf_fresh_left(&g, ramen) && g.bite_ms == 339 && g.red_ms == 6501 &&
        g.phase == BF_DINING);
  bf_step(&g, 498, 0, 0);
  CHECK(g.phase == BF_DINING && g.red_ms == 6999);
  bf_step(&g, 1, 0, 0);
  CHECK(g.phase == BF_HOSPITAL && g.purse == 8 && g.elapsed_ms == 26499);
}

static void leave_and_buy_again(void) {
  bf_game_t g = seated();
  g.en = true;
  g.tasting = true;
  g.cover = 3;
  g.muted = true;
  g.album.owned = 2;
  g.album.reduced_motion = true;
  g.album.unsaved = true;
  g.player.pos = bf_place(&g, 4).station;
  tap(&g, bf_eat_bounds);
  CHECK(g.eaten == 1 && g.bite_ms > 0);
  bf_action(&g, H2_BUFFET_MENU | H2_BUFFET_CONFIRM);
  CHECK(g.eaten == 1 && g.meal_menu == BF_MEAL_STATUS);
  bf_step(&g, 9000, 0, 0);
  CHECK(g.bite_ms == 680 && !g.album.awaiting);
  /* A repeated touch at the first Leave location backs out. A new deliberate
   * press on the other side is required to commit the departure. */
  tap(&g, bf_pause_left);
  tap(&g, bf_pause_left);
  CHECK(g.meal_menu == BF_MEAL_STATUS);
  tap(&g, bf_pause_left);
  contact(&g, H2_PAL_TOUCH_EVENT_DOWN, 0, bf_pause_right);
  contact(&g, H2_PAL_TOUCH_EVENT_CANCEL, 0, bf_pause_right);
  CHECK(g.meal_menu == BF_CONFIRM_LEAVE);
  tap(&g, bf_pause_right);
  CHECK(g.phase == BF_TICKET && g.left_early && g.meal_menu == BF_LIVE);
  CHECK(g.purse == 32 && g.album.owned == 2 && !g.album.fresh &&
        !g.album.awaiting);
  contact(&g, H2_PAL_TOUCH_EVENT_UP, 0, bf_ticket_button);
  CHECK(g.phase == BF_TICKET);
  tap(&g, bf_ticket_button);
  CHECK(g.phase == BF_DINING && !g.left_early && g.elapsed_ms == 0);
  CHECK(g.purse == 32 && !g.eaten && !g.value && !g.fullness_milli &&
        !g.red_ms && !g.bite_ms);
  CHECK(g.served == 4 && g.album.owned == 2 && !g.album.fresh &&
        g.album.reduced_motion && g.album.unsaved);
  CHECK(g.en && g.tasting && g.muted && g.cover == 3);
}

static void keys_and_closing(void) {
  bf_game_t g = seated();
  bf_point_t foot = g.player.pos;
  for (int i = 0; i < 3; i++) {
    bf_action(&g, H2_BUFFET_MENU);
    bf_step(&g, 50000, 1, 0);
    bf_action(&g, H2_BUFFET_MENU);
    bf_step(&g, 33, 1, 0);
    CHECK(!g.player.walking && g.player.pos.x == foot.x);
    CHECK(g.elapsed_ms == (i + 1) * 33);
  }
  bf_step(&g, 33, 0, 0);
  bf_step(&g, 33, 1, 0);
  CHECK(g.player.walking && g.player.pos.x > foot.x);
  g.elapsed_ms = bf_rules.duration_ms - 1;
  bf_action(&g, H2_BUFFET_MENU);
  bf_action(&g, H2_BUFFET_PREVIOUS | H2_BUFFET_CONFIRM);
  CHECK(g.meal_menu == BF_CONFIRM_LEAVE);
  bf_action(&g, H2_BUFFET_MENU);
  CHECK(g.meal_menu == BF_MEAL_STATUS);
  bf_step(&g, 2000, 0, 0);
  CHECK(g.phase == BF_DINING && !g.album.awaiting);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.meal_menu == BF_LIVE);
  bf_step(&g, 1, 0, 0);
  CHECK(g.phase == BF_CLOSING && g.album.awaiting == 2);
}
int main(void) {
  freeze_the_meal();
  leave_and_buy_again();
  keys_and_closing();
  puts(
      "Pause freezes the whole meal. Resume, departure, fresh admission, stale inputs, hospital and closing boundaries passed.");
  return 0;
}
