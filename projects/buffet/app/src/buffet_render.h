#ifndef BUFFET_RENDER_H
#define BUFFET_RENDER_H
#include "buffet_draw.h"

/* The owner caches the static ground for the room or the ticket cloth. */
void bf_paint_ground(bf_canvas_t c, bool dining);
void bf_paint_dining(bf_canvas_t c, const bf_game_t* g);
void bf_paint_scene(bf_canvas_t c, const bf_game_t* g);
#endif
