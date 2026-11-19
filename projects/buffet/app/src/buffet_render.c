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
static const bf_paint_t* const detail[] = {&bf_salmon, &bf_tamago, &bf_tuna,
                                           &bf_shrimp, &bf_maki,   &bf_inari};
static const bf_paint_t* const small[] = {&bf_salmon_table, &bf_tamago_table,
                                          &bf_tuna_table,   &bf_shrimp_table,
                                          &bf_maki_table,   &bf_inari_table};
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
           T("价值 %d 币    饱腹 +%d%%", "%d coins    +%d%% fullness"),
           bf_menu[d].coins, bf_menu[d].fullness);
  bf_text(c, BF_SMALL, 35, 383, patina, s);
  for (int i = 0; i < 6; i++) {
    int x = 24 + i * 70;
    bf_layer(c, small[i], (bf_rect_t){x + 2, 397, 66, 43}, false);
    if (i == d) bf_box(c, x + 10, 446, 47, 2, C(0xefc589));
  }
  ticket(c);
  bf_text(c, BF_HEAD, 502, 102, ink, T("用餐券", "Meal ticket"));
  bf_line(c, 501, 114, 737, 114, C(0xbba882));
  bf_text(c, BF_SMALL, 502, 148, mute, T("用餐时间", "Dining time"));
  clock_string(s, sizeof(s), bf_rules.duration_ms);
  right(c, BF_BODY, 737, 149, ink, s);
  bf_text(c, BF_SMALL, 502, 179, mute, T("随身餐费", "Meal purse"));
  snprintf(s, sizeof(s), T("%d 币", "%d coins"), bf_rules.purse);
  right(c, BF_BODY, 737, 179, ink, s);
  bf_text(c, BF_SMALL, 502, 215, mute, T("这顿想试试", "Meal goal"));
  slab(c, bf_goal_button, C(0xdac9a7));
  bf_center(c, BF_BODY, 620, 250, ink,
            g->tasting ? T("尝遍六种", "Taste all six")
                       : T("吃回票价", "Cover the ticket"));
  bf_line(c, 718, 243, 722, 247, mute);
  bf_line(c, 722, 247, 726, 243, mute);
  bf_text(c, BF_SMALL, 502, 288, mute,
          g->keyboard ? T("点地面或方向键行走，点吃或按空格。",
                          "Walk by tap or arrow keys. Eat with Space.")
                      : T("点地面行走，靠近餐桌点吃。",
                          "Tap to walk. Eat beside a table."));
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
static int index_at(unsigned mask, int slot) {
  for (int i = 0; i < 3; i++)
    if (mask & (1u << i)) {
      if (slot-- == 0) return i;
    }
  return 0;
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
      g->tasting
          ? (g->varieties == 6 ? T("已尝遍六种", "All six tasted")
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
            T("本局没有新卡", "No new cards this meal"));
  for (int i = 0; i < count; i++) {
    int card = index_at(a->fresh, i), x = 38 + i * 74;
    if (a->reveal_ms >= (i + 1) * BF_REVEAL_MS)
      bf_image(c, prints[card], x, 354, 46, 59);
    else
      bf_image(c, &bf_card_back, x, 354, 46, 59);
  }
  if (revealing) {
    int cur = a->reveal_ms / BF_REVEAL_MS, id = index_at(a->fresh, cur);
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
  if (!a->this_meal || mask & (1u << id)) {
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
static void sound_toggle(bf_canvas_t c, const bf_game_t* g) {
  bf_rect_t b = bf_sound_bounds(g);
  bool play = g->phase == BF_DINING;
  int x = b.x + (play ? 16 : 4), y = b.y + (play ? 17 : 18);
  if (play) bf_ellipse(c, x + 4, y, 11, 10, C(0x3d4b42), 225);
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
  if (g->album.open)
    album(c, g);
  else if (g->phase == BF_DINING)
    bf_paint_dining(c, g);
  else if (g->phase == BF_TICKET)
    foyer(c, g);
  else
    receipt(c, g);
  sound_toggle(c, g);
}
