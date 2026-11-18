#ifndef H2_FRUIT_SLICE_CARDS_RENDER_H
#define H2_FRUIT_SLICE_CARDS_RENDER_H

#include "h2_fruit_slice_render.h"

/* Private shared primitives. Measurement records coverage without writing. */
typedef struct h2_fruit_slice_region {
  int32_t x0, y0, x1, y1;
} h2_fruit_slice_region_t;

bool h2_fruit_slice_canvas_region(h2_fruit_slice_canvas_t* canvas, float x0,
                                  float y0, float x1, float y1,
                                  h2_fruit_slice_region_t* out);
uint16_t* h2_fruit_slice_canvas_row(const h2_fruit_slice_canvas_t* canvas,
                                    int32_t y);
uint16_t h2_fruit_slice_blend(uint16_t dst, uint16_t src, uint32_t alpha);
void h2_fruit_slice_draw_disc(h2_fruit_slice_canvas_t* canvas, float cx,
                              float cy, float r, uint16_t color,
                              uint32_t alpha);
void h2_fruit_slice_draw_capsule(h2_fruit_slice_canvas_t* canvas, float x0,
                                 float y0, float x1, float y1, float r,
                                 uint16_t color, uint32_t alpha);
void h2_fruit_slice_draw_rounded_rect(h2_fruit_slice_canvas_t* canvas, float x0,
                                      float y0, float x1, float y1,
                                      float radius, uint16_t color,
                                      uint32_t alpha);
void h2_fruit_slice_draw_text(h2_fruit_slice_canvas_t* canvas,
                              const char* value, int32_t x, int32_t y,
                              int32_t scale, uint16_t color, uint32_t alpha);
void h2_fruit_slice_draw_text_centered(h2_fruit_slice_canvas_t* canvas,
                                       const char* value, float cx, float y,
                                       int32_t scale, uint16_t color,
                                       uint32_t alpha);
int32_t h2_fruit_slice_text_scale(float unit, float base);
int32_t h2_fruit_slice_fit_scale(const char* value, float max_width,
                                 int32_t preferred);
void h2_fruit_slice_draw_fruit(h2_fruit_slice_canvas_t* canvas,
                               h2_fruit_slice_kind_t kind, float cx, float cy,
                               float r, float angle, uint64_t time_ms);
bool h2_fruit_slice_sprites_prepare_fruit(h2_fruit_slice_sprites_t* sprites,
                                          h2_fruit_slice_kind_t kind, float r);
bool h2_fruit_slice_cards_prepare_sprites(h2_fruit_slice_sprites_t* sprites,
                                          const h2_fruit_slice_game_t* game);
void h2_fruit_slice_cards_render_badge(h2_fruit_slice_canvas_t* canvas,
                                       const h2_fruit_slice_game_t* game);
/* Occupied height including drop shadow; zero without earned cards. */
int32_t h2_fruit_slice_cards_result_height(const h2_fruit_slice_game_t* game);
void h2_fruit_slice_cards_render_result(h2_fruit_slice_canvas_t* canvas,
                                        const h2_fruit_slice_game_t* game,
                                        float y);
void h2_fruit_slice_cards_render_play_scene(h2_fruit_slice_canvas_t* canvas,
                                            const h2_fruit_slice_game_t* game);
void h2_fruit_slice_cards_render_reveal_static(
    h2_fruit_slice_canvas_t* canvas, const h2_fruit_slice_game_t* game);
void h2_fruit_slice_cards_render_reveal_scene(
    h2_fruit_slice_canvas_t* canvas, const h2_fruit_slice_game_t* game);
void h2_fruit_slice_cards_render_album_static(
    h2_fruit_slice_canvas_t* canvas, const h2_fruit_slice_game_t* game);
void h2_fruit_slice_cards_render_album_scene(h2_fruit_slice_canvas_t* canvas,
                                             const h2_fruit_slice_game_t* game);

#endif
