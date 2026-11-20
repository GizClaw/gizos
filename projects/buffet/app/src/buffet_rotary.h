#ifndef BUFFET_ROTARY_H
#define BUFFET_ROTARY_H

#include "buffet_game.h"

typedef struct {
  float u, v;
} bf_world_t;
typedef struct {
  float u, v, w, h, r;
} bf_footprint_t;
typedef struct {
  bf_world_t point, normal;
} bf_mark_t;
extern const bf_footprint_t bf_islands[BF_LOOPS];
extern const bf_footprint_t bf_counters[];
extern const unsigned bf_counter_count;
extern const int bf_laps[BF_LOOPS];
bf_point_t bf_project(bf_world_t world, float height);
bf_world_t bf_unproject(bf_point_t point);
float bf_floor_height(bf_world_t point);
bf_footprint_t bf_inset(bf_footprint_t rect, float offset);
bf_mark_t bf_perimeter(bf_footprint_t rect, float phase);
float bf_loop_phase(int loop, int elapsed_ms);
bool bf_inside(bf_world_t p, bf_footprint_t rect, float margin);
#endif
