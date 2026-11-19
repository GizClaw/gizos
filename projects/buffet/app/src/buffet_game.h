#ifndef BUFFET_GAME_H
#define BUFFET_GAME_H

#include "h2_buffet.h"

enum {
  BF_TABLES = 3,
  BF_SETTINGS = 3,
  BF_SLOTS = BF_TABLES * BF_SETTINGS,
  BF_DISHES = 6,
  BF_ROUTE = 16,
  BF_CARD_COUNT = 3,
  BF_REVEAL_MS = 1000,
  BF_FLOOR_X = 40,
  BF_FLOOR_Y = 72,
  BF_TILE_W = 48,
  BF_TILE_H = 36,
  BF_PERSON_W = 64,
  BF_PERSON_H = 72,
  BF_PERSON_FOOT = 65,
  BF_PERSON_MOUTH = 24,
  BF_COLS = 15,
  BF_ROWS = 10,
  BF_TABLE_RISE = 22
};
typedef struct {
  unsigned owned, fresh, awaiting;
  int reveal_ms, selected;
  bool open, this_meal, reduced_motion, unsaved;
} bf_album_t;
typedef enum { BF_TICKET, BF_DINING, BF_CLOSING, BF_HOSPITAL } bf_phase_t;
typedef enum { BF_DOWN, BF_RIGHT, BF_UP, BF_LEFT } bf_face_t;
typedef struct {
  float x, y;
} bf_point_t;
typedef struct {
  int x, y, w, h;
} bf_rect_t;
typedef struct {
  const char* zh;
  const char* en;
} bf_text_t;
typedef struct {
  const char* id;
  bf_text_t name, detail;
  int coins, fullness;
} bf_dish_t;
typedef struct {
  int duration_ms, purse, ticket, fee, capacity, red_line, tolerance_ms,
      digest_milli_per_ms;
} bf_rules_t;
typedef struct {
  bf_point_t pos;
  bf_face_t face;
  float stride;
  bool walking;
} bf_actor_t;
typedef struct {
  bf_point_t pos, station;
} bf_place_t;
typedef struct {
  int col, row, period_ms, first_ms;
  bf_text_t name;
} bf_table_t;
/* dish is -1 at an empty setting. The kitchen waits for space at a full table.
 */
typedef struct {
  int dish, arrived_ms;
} bf_setting_t;
typedef struct {
  int next_at_ms, course, cursor;
} bf_service_t;
typedef struct {
  bf_phase_t phase;
  bf_album_t album;
  float gauge_milli;
  bool en, tasting, keyboard, muted;
  int cover, elapsed_ms, ambient_ms, receipt_ms, red_ms, recovered_ms;
  int fullness_milli, value, purse, eaten, returned;
  unsigned red_entries;
  int bites[BF_DISHES], varieties, last_dish, bite_ms, turn_ms, feedback_ms;
  bf_setting_t settings[BF_SLOTS];
  bf_service_t service[BF_TABLES];
  bf_actor_t player;
  bf_point_t bite_source;
  bf_point_t route[BF_ROUTE];
  int route_len, route_at;
} bf_game_t;

extern const bf_rules_t bf_rules;
extern const bf_dish_t bf_menu[BF_DISHES];
extern const bf_table_t bf_tables[BF_TABLES];
extern const bf_rect_t bf_language_button, bf_goal_button, bf_ticket_button;
extern const bf_rect_t bf_album_button, bf_round_cards, bf_album_back,
    bf_album_tab;
void bf_award(bf_game_t* g);
void bf_confirm_cards(bf_album_t* album, unsigned new_in_store);
unsigned bf_visible_cards(const bf_game_t* g);
int bf_card_position(const bf_game_t* g, int index);
int bf_card_count(unsigned mask);
int bf_reveal_duration(const bf_game_t* g);
void bf_init(bf_game_t* g, bool keyboard);
void bf_action(bf_game_t* g, unsigned actions);
void bf_tap(bf_game_t* g, float x, float y);
void bf_step(bf_game_t* g, int dt_ms, int dx, int dy);
bf_rect_t bf_sound_bounds(const bf_game_t* g);
bf_rect_t bf_eat_bounds(const bf_game_t* g, int slot);
bool bf_has_goal(const bf_game_t* g);
bool bf_in(bf_rect_t b, float x, float y);
bf_rect_t bf_table_footprint(int table);
bf_place_t bf_place(const bf_game_t* g, int slot);
bool bf_can_take(const bf_game_t* g, int slot);
bf_face_t bf_facing(float dx, float dy);
int bf_nearest(const bf_game_t* g);
int bf_next_tables(const bf_game_t* g, int order[BF_TABLES]);
int bf_next_setting(const bf_game_t* g, int table);
int bf_next_dish(const bf_game_t* g, int table);
bool bf_walkable(bf_point_t p);
void bf_service_open(bf_game_t* g);
void bf_service_update(bf_game_t* g);
void bf_service_clear(bf_game_t* g, int slot);
void bf_walk_to(bf_game_t* g, bf_point_t dest);
void bf_walk(bf_game_t* g, int dt_ms, int dx, int dy);
#endif
