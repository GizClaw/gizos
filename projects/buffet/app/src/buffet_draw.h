#ifndef BUFFET_DRAW_H
#define BUFFET_DRAW_H
#include "buffet_game.h"
#include "buffet_sprite.h"

typedef struct {
  uint16_t* pixels;
  bool en;
  uint8_t* heights; /* Half-pixel heights of cached architecture. */
  float foot_depth; /* A billboard uses its sole's unraised screen y. */
} bf_canvas_t;
typedef enum { BF_SMALL, BF_BODY, BF_HEAD, BF_TITLE } bf_size_t;
#define BF_COLOR(rgb)                                                  \
  ((uint16_t)((((rgb) >> 19) & 31) << 11 | (((rgb) >> 10) & 63) << 5 | \
              (((rgb) >> 3) & 31)))
void bf_veil(bf_canvas_t c, uint16_t color);
void bf_box(bf_canvas_t c, int x, int y, int w, int h, uint16_t color);
bool bf_depth_visible(bf_canvas_t c, int physical_x, int physical_y);
void bf_ellipse(bf_canvas_t c, int x, int y, int rx, int ry, uint16_t color,
                unsigned alpha);
void bf_line(bf_canvas_t c, int x1, int y1, int x2, int y2, uint16_t color);
void bf_blit(bf_canvas_t c, const bf_sprite_t* s, bf_rect_t src, bf_rect_t dst);
void bf_layer(bf_canvas_t c, const bf_paint_t* layer, bf_rect_t dst,
              bool mirrored);
void bf_layer_opacity(bf_canvas_t c, const bf_paint_t* layer, bf_rect_t dst,
                      unsigned opacity);
void bf_image(bf_canvas_t c, const bf_sprite_t* image, int x, int y, int w,
              int h);
/* Tilt is the horizontal displacement from the top edge to the bottom. */
void bf_print_tilted(bf_canvas_t c, const bf_sprite_t* image, bf_rect_t dst,
                     int tilt);
int bf_text(bf_canvas_t c, bf_size_t size, int x, int baseline, uint16_t color,
            const char* s);
int bf_width(bool en, bf_size_t size, const char* s);
bool bf_font_covers(bool en, bf_size_t size, const char* s);
void bf_center(bf_canvas_t c, bf_size_t size, int x, int baseline,
               uint16_t color, const char* s);
#endif
