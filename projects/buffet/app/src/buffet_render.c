#include "buffet_render.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define C(x) BF_COLOR(x)
#define T(zh, en_text) (c.en ? (en_text) : (zh))
static const uint16_t ink = C(0x273d3c), paper = C(0xf3e3bf),
                      light = C(0xf6ead0), sea = C(0x294c4b),
                      patina = C(0xaacaba), clay = C(0xa94f38),
                      mute = C(0x536457);
static const bf_paint_t* const detail[BF_DISHES] = {
    &bf_salmon, &bf_tamago, &bf_tuna,  &bf_shrimp,
    &bf_maki,   &bf_inari,  &bf_ramen, &bf_custard};
static const bf_paint_t* const small[] = {
    &bf_salmon_table, &bf_tamago_table, &bf_tuna_table,  &bf_shrimp_table,
    &bf_maki_table,   &bf_inari_table,  &bf_ramen_table, &bf_custard_table};
static const bf_sprite_t* const prints[] = {&bf_card_bite, &bf_card_close,
                                            &bf_card_rest};
static const bf_text_t card_names[] = {
    {"第一口", "First Bite"},
    {"顺利吃到打烊", "Made It to Closing"},
    {"第一次进病房", "First Trip to the Hospital"}};
static const bf_text_t card_notes[] = {
    {"在餐桌旁吃一份菜，顺利吃到打烊。",
     "Eat a serving at a table, then stay until closing."},
    {"凭票入座，等到餐厅收盘。",
     "Stay for a paid meal until the restaurant closes."},
    {"首次因持续超过危险线而住院。",
     "Be admitted after staying over the red line."}};
static const char* local(bf_canvas_t c, bf_text_t t) {
  return c.en ? t.en : t.zh;
}
static void right(bf_canvas_t c, bf_size_t size, int x, int y, uint16_t color,
                  const char* s) {
  bf_text(c, size, x - bf_width(c.en, size, s), y, color, s);
}
static void slab(bf_canvas_t c, bf_rect_t b, uint16_t color) {
  bf_box(c, b.x + 3, b.y, b.w - 6, b.h, color);
  bf_box(c, b.x, b.y + 3, b.w, b.h - 6, color);
}
static void btn(bf_canvas_t c, bf_rect_t b, uint16_t bg, const char* s) {
  slab(c, b, bg);
  bf_center(c, BF_BODY, b.x + b.w / 2, b.y + b.h / 2 + 6, light, s);
}
static void dotted(bf_canvas_t c, int x, int end, int y, uint16_t color) {
  for (; x < end; x += 7) bf_box(c, x, y, 3, 1, color);
}
static void clock_string(char* s, size_t n, int ms) {
  int t = (ms + 999) / 1000;
  snprintf(s, n, "%d:%02d", t / 60, t % 60);
}
static void ticket(bf_canvas_t c) {
  bf_box(c, 482, 59, 280, 376, C(0x244c4b));
  bf_box(c, 479, 56, 280, 376, paper);
  bf_box(c, 485, 64, 1, 357, C(0xd5b992));
  for (int x = 484; x < 754; x += 12) {
    bf_line(c, x, 431, x + 3, 428, C(0x42625a));
    bf_line(c, x + 3, 428, x + 6, 431, C(0x42625a));
  }
  for (int i = 0; i < 2; i++) bf_ellipse(c, i ? 759 : 479, 326, 6, 6, sea, 255);
  dotted(c, 494, 747, 326, C(0xac9474));
}
static void stack_mark(bf_canvas_t c, int x, int y) {
  bf_box(c, x + 2, y, 13, 15, C(0xbaa783));
  bf_box(c, x, y + 2, 13, 16, C(0xf0dbad));
  bf_box(c, x + 2, y + 4, 9, 12, C(0x547875));
  bf_line(c, x + 4, y + 11, x + 7, y + 8, light);
  bf_line(c, x + 7, y + 8, x + 9, y + 10, light);
}
static void house(bf_canvas_t c) {
  bf_image(c, &bf_mark, 26, 16, 42, 25);
  bf_text(c, BF_TITLE, 78, 42, light, T("潮轮寿司", "Tidewheel Sushi"));
}
static void album_entry(bf_canvas_t c) {
  stack_mark(c, 610, 21);
  bf_text(c, BF_SMALL, 632, 34, light, T("成就卡册", "Card album"));
}
static void foyer(bf_canvas_t c, const bf_game_t* g) {
  char s[96];
  int d = g->cover;
  house(c);
  album_entry(c);
  btn(c, bf_language_button, sea, T("EN", "中文"));
  for (int i = 6; i > 0; i--)
    bf_ellipse(c, 250, 228, 126 + 4 * i, 47 + 2 * i, C(0x102e32), 13);
  bf_layer(c, detail[d], (bf_rect_t){57, 68, 375, 244}, false);
  bf_text(c, BF_HEAD, 34, 335, light, local(c, bf_menu[d].name));
  bf_text(c, BF_SMALL, 35, 359, light, local(c, bf_menu[d].detail));
  snprintf(s, sizeof(s),
           T("价值 %d 币    饱腹 +%d%%    保鲜 %d 秒",
             "%d coins    +%d%% fullness    Fresh for %ds"),
           bf_menu[d].coins, bf_menu[d].fullness, bf_menu[d].fresh_seconds);
  bf_text(c, BF_SMALL, 35, 383, patina, s);
  for (int i = 0; i < BF_DISHES; i++) {
    int x = BF_MENU_X + i * BF_MENU_PITCH;
    bf_layer(c, small[i], (bf_rect_t){x, 396, BF_MENU_PITCH, 44}, false);
    if (i == d) bf_box(c, x + 8, 446, BF_MENU_PITCH - 16, 2, C(0xefc589));
  }
  ticket(c);
  bf_text(c, BF_HEAD, 502, 102, ink, T("用餐券", "Meal ticket"));
  bf_line(c, 501, 114, 737, 114, C(0xbba882));
  bf_text(c, BF_SMALL, 502, 148, mute, T("用餐时间", "Dining time"));
  clock_string(s, sizeof(s), bf_rules.duration_ms);
  right(c, BF_BODY, 737, 149, ink, s);
  bf_text(c, BF_SMALL, 502, 179, mute,
          g->left_early ? T("上局餐费余额", "Last purse")
                        : T("随身餐费", "Meal purse"));
  snprintf(s, sizeof(s), T("%d 币", "%d coins"), g->purse);
  right(c, BF_BODY, 737, 179, ink, s);
  if (g->left_early) {
    snprintf(s, sizeof(s), T("新局餐费 %d 币", "New meal purse: %d coins"),
             bf_rules.purse);
    bf_text(c, BF_SMALL, 502, 199, mute, s);
  }
  bf_text(c, BF_SMALL, 502, 215, mute, T("这顿想试试", "Meal goal"));
  slab(c, bf_goal_button, C(0xdac9a7));
  bf_center(c, BF_BODY, 620, 250, ink,
            g->tasting ? T("尝遍六种寿司", "Taste six sushi")
                       : T("吃回票价", "Cover the ticket"));
  bf_line(c, 718, 243, 722, 247, mute);
  bf_line(c, 722, 247, 726, 243, mute);
  bf_text(c, BF_SMALL, 502, 288, mute,
          g->keyboard ? T("摇杆或方向键移动，吃键或空格进食。",
                          "Stick or arrows. Bowl button or Space to eat.")
                      : T("拖左边摇杆，靠近餐盘按右边碗键。",
                          "Drag the stick. Near a plate, tap the bowl."));
  bf_text(c, BF_SMALL, 502, 311, mute,
          T("米碗白线是吃饱，尖格亮起就歇口。",
            "The white notch is full. Rest at the points."));
  snprintf(s, sizeof(s), T("买票入座   %d 币", "Buy ticket   %d coins"),
           bf_rules.ticket);
  btn(c, bf_ticket_button, clay, s);
}
static void row(bf_canvas_t c, int y, const char* name, int value,
                bool signed_value) {
  char s[16];
  snprintf(s, sizeof(s), signed_value ? "%+d" : "%d", value);
  bf_text(c, BF_SMALL, 502, y, mute, name);
  right(c, BF_BODY, 737, y, ink, s);
}
static float ease(float x) {
  float y = 1 - x;
  return 1 - y * y * y;
}
static void receipt(bf_canvas_t c, const bf_game_t* g) {
  const bf_album_t* a = &g->album;
  bool hospital = g->phase == BF_HOSPITAL;
  char s[100];
  house(c);
  album_entry(c);
  ticket(c);
  bf_text(c, BF_HEAD, 502, 101, ink,
          hospital ? T("住院账单", "Hospital bill")
                   : T("用餐小票", "Meal receipt"));
  bf_line(c, 502, 114, 737, 114, C(0xbba882));
  row(c, 148, T("吃菜价值", "Food value"), g->value, false);
  row(c, 177, T("实付票价", "Ticket paid"), bf_rules.ticket, false);
  row(c, 207, T("价值差额", "Value less ticket"), g->value - bf_rules.ticket,
      true);
  dotted(c, 503, 738, 219, C(0xb19b77));
  row(c, 247, T("住院费", "Hospital fee"), hospital ? bf_rules.fee : 0, false);
  row(c, 272, T("餐费余额", "Purse left"), g->purse, false);
  const char* outcome =
      g->tasting ? (bf_has_goal(g) ? T("已尝遍六种寿司", "All six sushi tasted")
                                   : T("还有新口味可尝", "More dishes to try"))
                 : (bf_has_goal(g) ? T("已吃回票价", "Ticket covered")
                                   : T("未吃回票价", "Below ticket value"));
  bf_text(c, BF_SMALL, 502, 305, ink, outcome);
  bool revealing = a->reveal_ms < bf_reveal_duration(g);
  btn(c, bf_ticket_button, clay,
      revealing ? T("收下新卡", "Collect cards") : T("再来一局", "Play again"));
  bf_ellipse(c, 157, 287, 66, 9, C(0x163d3c), 82);
  bf_image(c, hospital ? &bf_rest : &bf_guest, 44, 82, 207, 232);
  bf_text(c, BF_HEAD, 239, 130, light,
          hospital ? T("这顿吃撑了", "A little too full")
                   : T("打烊收盘", "Last plates home"));
  snprintf(s, sizeof(s),
           T("吃了 %d 盘，尝了 %d 种。", "%d plates, %d dishes tasted."),
           g->eaten, g->varieties);
  bf_text(c, BF_BODY, 240, 157, patina, s);
  snprintf(s, sizeof(s), T("回收空盘 %d 只", "%d empty plates cleared"),
           g->returned);
  bf_text(c, BF_SMALL, 240, 181, patina, s);
  for (int i = 0; i < (g->eaten > 3 ? 3 : g->eaten); i++)
    bf_layer(c, &bf_empty, (bf_rect_t){248, 215 - i * 3, 100, 65}, false);
  dotted(c, 31, 438, 314, C(0x78978a));
  bf_text(c, BF_BODY, 34, 345, light, T("本局成就", "This Round's Cards"));
  int count = bf_card_count(a->fresh);
  if (!count)
    bf_text(c, BF_SMALL, 35, 375, patina,
            a->awaiting && !a->unsaved
                ? T("正在整理本局卡片", "Checking your cards")
                : T("本局没有新卡", "No new cards this meal"));
  for (int i = 0; i < count; i++) {
    int card = bf_card_at(a->fresh, i), x = bf_rack_card(i).x + 8;
    if (a->reveal_ms >= (i + 1) * BF_REVEAL_MS)
      bf_image(c, prints[card], x, 354, 46, 59);
    else
      bf_image(c, &bf_card_back, x, 354, 46, 59);
  }
  if (revealing) {
    int cur = a->reveal_ms / BF_REVEAL_MS, id = bf_card_at(a->fresh, cur);
    float t = (a->reveal_ms % BF_REVEAL_MS) / (float)BF_REVEAL_MS,
          fly = ease(fmaxf(0, (t - .64f) / .36f));
    float flip = fminf(1, t / .40f);
    int w = (int)((104 - 58 * fly) * fabsf(cosf(flip * 3.14159f)));
    if (w < 2) w = 2;
    int h = (int)(134 - 75 * fly), x = (int)(312 + (61 + cur * 74 - 312) * fly),
        y = (int)(232 + (383 - 232) * fly);
    bf_image(c, flip < .5f ? &bf_card_back : prints[id], x - w / 2, y - h / 2,
             w, h);
  }
  bf_text(c, BF_SMALL, 244, 391, light, T("打开成就卡册", "Open card album"));
  if (a->unsaved)
    bf_text(c, BF_SMALL, 34, 448, light,
            T("卡册暂存于本次游戏", "Cards kept for this session"));
}
static void album(bf_canvas_t c, const bf_game_t* g) {
  const bf_album_t* a = &g->album;
  unsigned mask = a->this_meal ? a->fresh : a->owned;
  bf_text(c, BF_TITLE, 34, 59, light, T("成就卡册", "Card album"));
  btn(c, bf_album_back, sea, T("返回", "Back"));
  if (g->phase != BF_TICKET)
    btn(c, bf_album_tab, sea,
        a->this_meal
            ? T("本局新卡   /   查看全部", "This round    /    View all")
            : T("全部收藏   /   查看本局", "Collection    /    This round"));
  char s[80];
  snprintf(s, sizeof(s),
           a->this_meal ? T("本局新卡 %d 张", "%d new this meal")
                        : T("已收藏 %d 张", "%d collected"),
           bf_card_count(mask));
  bf_text(c, BF_BODY, 36, 87, patina, s);
  if (!mask && a->this_meal) {
    bf_center(c, BF_HEAD, 400, 234, light,
              T("本局没有新卡", "No new cards this meal"));
    return;
  }
  for (int i = 0; i < 3; i++) {
    int center = bf_card_position(g, i);
    bool owned = mask & (1u << i);
    if (!(bf_visible_cards(g) & (1u << i))) continue;
    bf_image(c, owned ? prints[i] : &bf_card_back, center - 56, 113, 112, 144);
    const char* title = !owned && i == 2 ? T("尚未发现", "Undiscovered")
                                         : local(c, card_names[i]);
    bf_center(c, BF_BODY, center, 287, light, title);
    if (i == a->selected) bf_box(c, center - 21, 303, 42, 2, C(0xedc48a));
  }
  int id = a->selected;
  if (id >= 0 && (!a->this_meal || mask & (1u << id))) {
    bf_center(c, BF_SMALL, 400, 360, patina,
              (mask & (1u << id)) ? T("已收藏", "Collected")
                                  : T("尚未收藏", "Not collected"));
    bf_center(c, BF_BODY, 400, 391, light,
              !(mask & (1u << id)) && id == 2
                  ? T("一次特别的用餐经历。", "A surprise during a meal.")
                  : local(c, card_notes[id]));
  }
  if (a->unsaved)
    bf_center(c, BF_SMALL, 400, 444, light,
              T("卡册暂存于本次游戏", "Cards kept for this session"));
}
static void inspect(bf_canvas_t c, const bf_game_t* g) {
  const bf_album_t* a = &g->album;
  int id = a->selected;
  bf_text(c, BF_TITLE, 34, 59, light,
          a->inspect_receipt ? T("本局成就", "This Round's Cards")
                             : T("成就卡册", "Card album"));
  btn(c, bf_inspect_back, sea, T("返回", "Back"));
  const float t = (float)a->inspect_ms / BF_INSPECT_MS, lift = ease(t);
  int sx = a->inspect_receipt ? bf_rack_card(a->inspect_slot).x + 31
                              : bf_card_position(g, id),
      sy = a->inspect_receipt ? 383 : 185;
  int sw = a->inspect_receipt ? 46 : 112, sh = a->inspect_receipt ? 59 : 144;
  int x = (int)lroundf(sx + (400 - sx) * lift),
      y = (int)lroundf(sy + (218 - sy) * lift);
  float turn = sinf(t * 3.14159265f) * (1 - t);
  int tilt = (int)lroundf((sx <= 400 ? 59 : -59) * turn),
      w = (int)lroundf(sw + (208 - sw) * lift - 25 * turn),
      h = (int)lroundf(sh + (268 - sh) * lift);
  bf_print_tilted(c, prints[id], (bf_rect_t){x - w / 2, y - h / 2, w, h}, tilt);
  if (t >= .55f) {
    bf_center(c, BF_HEAD, 400, 391, light, local(c, card_names[id]));
    bf_center(c, BF_BODY, 400, 421, patina, local(c, card_notes[id]));
  }
}
static void dining_menu_button(bf_canvas_t c) {
  /* A folded house menu with three inked strokes, beside the closing clock. */
  int x = bf_menu_button.x + bf_menu_button.w / 2, y = 28;
  bf_ellipse(c, x, y + 1, 19, 19, C(0x9a875e), 255);
  bf_ellipse(c, x, y, 18, 18, sea, 255);
  bf_box(c, x - 8, y - 10, 16, 21, light);
  bf_box(c, x - 6, y - 10, 2, 21, C(0xc9ae7e));
  for (int i = 0; i < 3; ++i)
    bf_box(c, x - 2, y - 5 + 4 * i, i == 2 ? 5 : 7, 2, sea);
}

static void pause_row(bf_canvas_t c, int y, const char* label,
                      const char* value) {
  bf_text(c, BF_HEAD, 254, y, ink, label);
  right(c, BF_HEAD, 558, y, ink, value);
}

static void meal_sheet(bf_canvas_t c, const bf_game_t* g) {
  bool confirming = g->meal_menu == BF_CONFIRM_LEAVE;
  bool danger = g->fullness_milli > bf_rules.red_line * 1000;
  char number[72];
  bf_veil(c, C(0x0f292a));
  /* A paper service slip in the same printed stock as the meal ticket. */
  slab(c, (bf_rect_t){199, 48, 408, 366}, C(0x385754));
  slab(c, (bf_rect_t){196, 44, 408, 366}, paper);
  bf_box(c, 204, 50, 1, 352, C(0xd5b992));
  bf_ellipse(c, 240, 78, 19, 19, sea, 255);
  bf_image(c, &bf_mark, 225, 69, 31, 19);
  bf_text(c, BF_TITLE, 270, 92, ink,
          confirming ? T("提前离席？", "Leave this meal?")
                     : T("用餐暂停", "Meal paused"));
  bf_line(c, 222, 109, 578, 109, C(0xb9a77f));
  if (confirming) {
    snprintf(number, sizeof(number),
             T("本局票款 %d 币已使用。", "Your %d-coin ticket stays spent."),
             bf_rules.ticket);
    bf_text(c, BF_HEAD, 238, 166, ink, number);
    bf_text(c, BF_BODY, 238, 201, ink,
            T("用餐到打烊才能领取打烊成就。",
              "Finish the meal to earn closing cards."));
    snprintf(number, sizeof(number), T("%d 币", "%d coins"), g->purse);
    pause_row(c, 270, T("本局餐费余额", "Purse left"), number);
  } else {
    bf_layer(c, &bf_hud_clock, (bf_rect_t){223, 121, 25, 25}, false);
    clock_string(number, sizeof(number), bf_rules.duration_ms - g->elapsed_ms);
    pause_row(c, 142, T("剩余时间", "Time left"), number);
    bf_layer(c, &bf_hud_plates, (bf_rect_t){222, 157, 27, 25}, false);
    snprintf(number, sizeof(number), "%d", g->eaten);
    pause_row(c, 178, T("已吃盘数", "Plates eaten"), number);
    bf_ellipse(c, 236, 208, 10, 7, C(0x966e46), 255);
    bf_ellipse(c, 236, 205, 10, 7, C(0xb78b51), 255);
    bf_ellipse(c, 236, 204, 7, 4, C(0xe7c987), 255);
    snprintf(number, sizeof(number), T("%d / %d 币", "%d / %d coins"), g->value,
             bf_rules.ticket);
    pause_row(c, 214, T("餐值 / 票价", "Value / ticket"), number);
    bf_blit(c, &bf_gauge, (bf_rect_t){0, 0, 70, 58},
            (bf_rect_t){222, 229, 27, 23});
    snprintf(number, sizeof(number), "%d%%", (g->fullness_milli + 500) / 1000);
    pause_row(c, 250, T("饱腹", "Fullness"), number);
    if (danger) {
      snprintf(
          number, sizeof(number),
          T("危险 · 恢复后 %d 秒送医", "Danger · Hospital in %ds after resume"),
          (bf_rules.tolerance_ms - g->red_ms + 999) / 1000);
      slab(c, (bf_rect_t){225, 262, 350, 31}, C(0x873d34));
      bf_center(c, BF_BODY, 400, 283, light, number);
    }
  }
  dotted(c, 222, 578, 306, C(0xb69b75));
  bf_ellipse(c, 196, 306, 5, 5, sea, 255);
  bf_ellipse(c, 604, 306, 5, 5, sea, 255);
  const char* left =
      confirming ? T("返回菜单", "Back") : T("提前离席", "Leave meal");
  const char* right_label =
      confirming ? T("确认离席", "Leave meal") : T("继续用餐", "Continue");
  slab(c, bf_pause_left, C(0xdccba8));
  bf_center(c, BF_HEAD, bf_pause_left.x + bf_pause_left.w / 2,
            bf_pause_left.y + bf_pause_left.h / 2 + 7, ink, left);
  slab(c, bf_pause_right, confirming ? clay : sea);
  bf_center(c, BF_HEAD, bf_pause_right.x + bf_pause_right.w / 2,
            bf_pause_right.y + bf_pause_right.h / 2 + 7, light, right_label);
  if (g->keyboard) {
    bool right_focused = confirming ? g->leave_selected : !g->leave_selected;
    bf_rect_t focus = right_focused ? bf_pause_right : bf_pause_left;
    bf_box(c, focus.x + focus.w / 2 - 14, focus.y + focus.h - 9, 28, 2,
           right_focused ? light : ink);
  }
}

static void sound_toggle(bf_canvas_t c, const bf_game_t* g) {
  bf_rect_t b = bf_sound_bounds(g);
  bool play = g->phase == BF_DINING;
  int x = b.x + (play ? b.w / 2 - 4 : 4), y = b.y + (play ? b.h / 2 - 1 : 18);
  if (play) bf_ellipse(c, x + 4, y, 18, 18, C(0x294c4b), 255);
  bf_box(c, x - 4, y - 2, 4, 5, light);
  for (int i = 0; i < 5; i++) bf_box(c, x + i, y - 2 - i / 2, 1, 5 + i, light);
  if (g->muted) {
    bf_line(c, x - 5, y + 6, x + 10, y - 6, C(0xf2ad8b));
  } else {
    bf_line(c, x + 7, y - 3, x + 9, y - 1, light);
    bf_line(c, x + 9, y - 1, x + 9, y + 1, light);
    bf_line(c, x + 9, y + 1, x + 7, y + 3, light);
  }
  if (!play)
    bf_text(c, BF_SMALL, x + 15, y + 5, light,
            g->muted ? T("静音", "Muted") : T("声音", "Sound"));
}
void bf_paint_scene(bf_canvas_t c, const bf_game_t* g) {
  c.en = g->en;
  if (g->album.inspecting) {
    inspect(c, g);
    return;
  }
  if (g->album.open)
    album(c, g);
  else if (g->phase == BF_DINING) {
    bf_paint_dining(c, g);
    if (g->meal_menu != BF_LIVE) {
      meal_sheet(c, g);
      return;
    }
    dining_menu_button(c);
  } else if (g->phase == BF_TICKET)
    foyer(c, g);
  else
    receipt(c, g);
  sound_toggle(c, g);
}
