#ifndef BUFFET_GAME_H
#define BUFFET_GAME_H

#include "h2_buffet.h"

enum {
  BF_SLOTS = 10,
  BF_DISHES = 6,
  BF_ROUTE = 8,
  BF_CARD_COUNT = 3,
  BF_REVEAL_MS = 1000
};
typedef struct {
  unsigned owned, fresh;
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
      digest_milli_per_ms, lap_ms;
} bf_rules_t;
typedef struct {
  bf_point_t pos;
  bf_face_t face;
  float stride;
  bool walking;
} bf_actor_t;
typedef struct {
  bf_point_t pos, station, normal;
} bf_place_t;
typedef struct {
  bf_phase_t phase;
  bf_album_t album;
  float gauge_milli;
  bool en, tasting, keyboard;
  int cover, elapsed_ms, ambient_ms, receipt_ms, red_ms, recovered_ms,
      fullness_milli, value, purse, eaten, returned;
  int bites[BF_DISHES], varieties, last_dish, bite_ms, turn_ms, feedback_ms;
  bool empty[BF_SLOTS], obstructed;
  bf_actor_t player, diners[2];
  bf_point_t route[BF_ROUTE];
  int route_len, route_at;
} bf_game_t;

extern const bf_rules_t bf_rules;
extern const bf_dish_t bf_menu[BF_DISHES];
extern const bf_rect_t bf_language_button, bf_goal_button, bf_ticket_button,
    bf_eat_button;
void bf_award(bf_game_t* g);
unsigned bf_visible_cards(const bf_game_t* g);
int bf_card_position(const bf_game_t* g, int index);
int bf_card_count(unsigned mask);
int bf_reveal_duration(const bf_game_t* g);
extern const bf_rect_t bf_album_button, bf_round_cards, bf_album_back,
    bf_album_tab;
void bf_init(bf_game_t* g, bool keyboard);
void bf_action(bf_game_t* g, unsigned actions);
void bf_tap(bf_game_t* g, float x, float y);
void bf_step(bf_game_t* g, int dt_ms, int dx, int dy);
bf_place_t bf_belt(float lap);
bf_place_t bf_place(const bf_game_t* g, int slot);
int bf_serving(const bf_game_t* g, int slot);
int bf_nearest(const bf_game_t* g);
bool bf_has_goal(const bf_game_t* g);
bool bf_at_belt(const bf_game_t* g);
bool bf_walkable(bf_point_t p);
bool bf_in(bf_rect_t b, float x, float y);
#endif
