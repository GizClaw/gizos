#include "h2_garden_guard_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                               \
  do {                                                                 \
    if (!(condition)) {                                                \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, \
              #condition);                                             \
      exit(1);                                                         \
    }                                                                  \
  } while (0)

#define MAX_PIXELS (480 * 800)

static uint16_t background[MAX_PIXELS];
static uint16_t pixels[MAX_PIXELS];
static uint16_t static_pixels[MAX_PIXELS];
static uint16_t unoccluded[MAX_PIXELS];
static uint8_t depth[MAX_PIXELS];
static h2_garden_guard_game_t game;

static h2_garden_guard_canvas_t canvas_for(uint16_t* target, bool measure) {
  return (h2_garden_guard_canvas_t){.pixels = target,
                                    .width = game.layout.width,
                                    .height = game.layout.height,
                                    .stride_pixels = (size_t)game.layout.width,
                                    .measure = measure};
}

static void init(int32_t width, int32_t height) {
  h2_garden_guard_game_init(&game, width, height, 5u, 0u);
  h2_garden_guard_canvas_t canvas = canvas_for(background, false);
  h2_garden_guard_render_background(&canvas, &game);
  memcpy(pixels, background, sizeof(pixels));
}

static uint16_t at(const uint16_t* image, float x, float y) {
  return image[(size_t)y * (size_t)game.layout.width + (size_t)x];
}

static void start_playing(void) {
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_DOWN, 10.0f,
                               10.0f);
  h2_garden_guard_game_pointer(&game, H2_GARDEN_GUARD_POINTER_UP, 10.0f, 10.0f);
  CHECK(game.phase == H2_GARDEN_GUARD_PHASE_PLAYING);
  game.banner_ms = 0;
}

/* Sum of the red channel over a block, to compare areas despite speckles. */
static uint32_t red_sum(float cx, float cy) {
  uint32_t sum = 0;
  for (int dy = -4; dy < 4; ++dy)
    for (int dx = -4; dx < 4; ++dx)
      sum += at(background, cx + (float)dx, cy + (float)dy) >> 11;
  return sum;
}

static void test_helpers(void) {
  CHECK(h2_garden_guard_all_bands(0) == 0u);
  CHECK(h2_garden_guard_all_bands(320) == ((uint64_t)1 << 20) - 1u);
  CHECK(h2_garden_guard_all_bands(1024) == UINT64_MAX);
  CHECK(h2_garden_guard_text_width("AB", 2) == 22);
  CHECK(h2_garden_guard_text_width("", 3) == 0);
}

static void test_background(void) {
  init(480, 320);
  const h2_garden_guard_layout_t* l = &game.layout;
  float x0, y0, x1, y1;
  h2_garden_guard_field_point(l, 0u, 0.5f, &x0, &y0);
  h2_garden_guard_field_point(l, 0u, 1.5f, &x1, &y1);
  /* Neighboring cells form a checkerboard. */
  CHECK(red_sum(x0, y0) != red_sum(x1, y1));
  /* Toolbar wood and the house path differ from the lawn. */
  CHECK(red_sum(240.0f, l->bar_h * 0.3f) != red_sum(x0, y0));
  CHECK(red_sum(l->field_x * 0.5f, y0) != red_sum(x0, y0));
}

/* The measure pass must cover exactly the bands the paint pass covers. */
static void check_coverage(h2_garden_guard_part_t part, bool scene) {
  h2_garden_guard_canvas_t measure = canvas_for(pixels, true);
  h2_garden_guard_canvas_t paint = canvas_for(pixels, false);
  if (scene) {
    h2_garden_guard_render_scene(&measure, &game);
    h2_garden_guard_render_scene(&paint, &game);
  } else {
    h2_garden_guard_render_static(&measure, &game, part);
    h2_garden_guard_render_static(&paint, &game, part);
  }
  CHECK(measure.bands == paint.bands);
  CHECK(memcmp(measure.band_x0, paint.band_x0, sizeof(measure.band_x0)) == 0);
  CHECK(memcmp(measure.band_x1, paint.band_x1, sizeof(measure.band_x1)) == 0);
}

static void test_static_parts(void) {
  init(480, 320);
  check_coverage(H2_GARDEN_GUARD_PART_FIELD, false);
  start_playing();
  memcpy(pixels, background, sizeof(pixels));
  const h2_garden_guard_layout_t* l = &game.layout;
  h2_garden_guard_static_key_t toolbar, field, key;
  h2_garden_guard_static_key(&game, H2_GARDEN_GUARD_PART_TOOLBAR, &toolbar);
  h2_garden_guard_static_key(&game, H2_GARDEN_GUARD_PART_FIELD, &field);

  /* Sun changes the toolbar only. */
  game.sun += 25u;
  h2_garden_guard_static_key(&game, H2_GARDEN_GUARD_PART_TOOLBAR, &key);
  CHECK(memcmp(&key, &toolbar, sizeof(key)) != 0);
  h2_garden_guard_static_key(&game, H2_GARDEN_GUARD_PART_FIELD, &key);
  CHECK(memcmp(&key, &field, sizeof(key)) == 0);

  /* Planting changes the field only, and draws the plant. */
  game.plants[1][2] = (h2_garden_guard_plant_t){
      .kind = H2_GARDEN_GUARD_PLANT_PUMPKIN, .hp = 2400.0f};
  ++game.plant_revision;
  h2_garden_guard_static_key(&game, H2_GARDEN_GUARD_PART_FIELD, &key);
  CHECK(memcmp(&key, &field, sizeof(key)) != 0);
  check_coverage(H2_GARDEN_GUARD_PART_FIELD, false);
  check_coverage(H2_GARDEN_GUARD_PART_TOOLBAR, false);
  float x, y;
  h2_garden_guard_field_point(l, 1u, 2.5f, &x, &y);
  CHECK(at(pixels, x + l->sprite * 0.2f, y) !=
        at(background, x + l->sprite * 0.2f, y));
  h2_garden_guard_field_point(l, 3u, 6.5f, &x, &y);
  CHECK(at(pixels, x, y) == at(background, x, y));

  /* The toolbar never draws into the lawn, and the field part never draws
   * into the toolbar. */
  memcpy(pixels, background, sizeof(pixels));
  h2_garden_guard_canvas_t canvas = canvas_for(pixels, false);
  h2_garden_guard_render_static(&canvas, &game, H2_GARDEN_GUARD_PART_TOOLBAR);
  const size_t bar_rows = (size_t)l->bar_h;
  CHECK(memcmp(&pixels[bar_rows * 480u], &background[bar_rows * 480u],
               (320u - bar_rows) * 480u * sizeof(uint16_t)) == 0);
  memcpy(pixels, background, sizeof(pixels));
  canvas = canvas_for(pixels, false);
  h2_garden_guard_render_static(&canvas, &game, H2_GARDEN_GUARD_PART_FIELD);
  CHECK(memcmp(pixels, background, bar_rows * 480u * sizeof(uint16_t)) == 0);

  /* The result prompt appears only after the restart delay. */
  game.phase = H2_GARDEN_GUARD_PHASE_LOST;
  game.phase_started_ms = game.time_ms;
  h2_garden_guard_static_key(&game, H2_GARDEN_GUARD_PART_FIELD, &field);
  CHECK(field.values[5] == 0u);
  game.time_ms += H2_GARDEN_GUARD_RESTART_DELAY_MS;
  h2_garden_guard_static_key(&game, H2_GARDEN_GUARD_PART_FIELD, &key);
  CHECK(key.values[5] == 1u);
  check_coverage(H2_GARDEN_GUARD_PART_FIELD, false);
}

static void test_scene(void) {
  init(480, 320);
  start_playing();
  const h2_garden_guard_layout_t* l = &game.layout;
  /* An empty field draws nothing. */
  h2_garden_guard_canvas_t canvas = canvas_for(pixels, true);
  h2_garden_guard_render_scene(&canvas, &game);
  CHECK(canvas.bands == 0u);

  game.pests[0] =
      (h2_garden_guard_pest_t){.active = true,
                               .kind = H2_GARDEN_GUARD_PEST_SHELLBACK,
                               .lane = 3u,
                               .x = 5.0f,
                               .hp = 190,
                               .armor = 330};
  game.pests[1] = (h2_garden_guard_pest_t){.active = true,
                                           .kind = H2_GARDEN_GUARD_PEST_RUNNER,
                                           .lane = 0u,
                                           .x = 7.0f,
                                           .hp = 110,
                                           .flash_ms = 50u};
  game.peas[0] = (h2_garden_guard_pea_t){.active = true, .lane = 3u, .x = 2.0f};
  game.suns[0] = (h2_garden_guard_sun_t){
      .active = true, .x = 200.0f, .y = 150.0f, .rest_y = 150.0f};
  game.carts[1] = (h2_garden_guard_cart_t){
      .state = H2_GARDEN_GUARD_CART_SWEEPING, .x = 4.0f};
  game.banner_ms = 1000u;
  (void)snprintf(game.banner, sizeof(game.banner), "WAVE 2");
  check_coverage(H2_GARDEN_GUARD_PART_FIELD, true);
  float x, y;
  h2_garden_guard_field_point(l, 3u, 5.0f, &x, &y);
  CHECK(at(pixels, x, y) != at(background, x, y));
  CHECK(at(pixels, 200.0f, 150.0f) != at(background, 200.0f, 150.0f));
}

/* A foreground plant must hide a pest in the previous lane without hiding
 * that pest's uncovered pixels. Every changed pixel must be transferable
 * using the measured dirty bands, including the raised head and shadows. */
static void test_depth_and_dirty_coverage(void) {
  /* A shallow landscape viewport makes adjacent upright figures overlap. */
  init(480, 240);
  start_playing();
  game.plants[4][4] = (h2_garden_guard_plant_t){
      .kind = H2_GARDEN_GUARD_PLANT_SUNFLOWER, .hp = 300.0f};
  memset(depth, 0, sizeof(depth));
  h2_garden_guard_canvas_t canvas = canvas_for(pixels, false);
  canvas.depth = depth;
  h2_garden_guard_render_static(&canvas, &game, H2_GARDEN_GUARD_PART_FIELD);
  memcpy(static_pixels, pixels, sizeof(pixels));
  game.pests[0] = (h2_garden_guard_pest_t){.active = true,
                                           .kind = H2_GARDEN_GUARD_PEST_BEETLE,
                                           .lane = 3u,
                                           .x = 4.5f,
                                           .hp = 190};
  canvas = canvas_for(pixels, false);
  h2_garden_guard_render_scene(&canvas, &game);
  memcpy(unoccluded, pixels, sizeof(pixels));
  memcpy(pixels, static_pixels, sizeof(pixels));
  h2_garden_guard_canvas_t measured = canvas_for(NULL, true);
  measured.depth = depth;
  h2_garden_guard_render_scene(&measured, &game);
  canvas = canvas_for(pixels, false);
  canvas.depth = depth;
  h2_garden_guard_render_scene(&canvas, &game);
  unsigned hidden = 0, visible = 0;
  for (int32_t y = 0; y < game.layout.height; ++y)
    for (int32_t x = 0; x < game.layout.width; ++x) {
      const size_t i = (size_t)y * (size_t)game.layout.width + (size_t)x;
      if (depth[i] == 5u) {
        CHECK(pixels[i] == static_pixels[i]);
        if (unoccluded[i] != static_pixels[i]) ++hidden;
      }
      if (pixels[i] != static_pixels[i]) {
        ++visible;
        const int band = y / H2_GARDEN_GUARD_BAND_ROWS;
        CHECK(measured.bands & ((uint64_t)1 << band));
        CHECK(x >= measured.band_x0[band] && x < measured.band_x1[band]);
      }
    }
  CHECK(hidden > 0u && visible > 0u);
  CHECK(measured.bands != h2_garden_guard_all_bands(game.layout.height));

  /* A moving foreground object can cover a plant in the same lane. */
  game.pests[0].lane = 4u;
  memcpy(pixels, static_pixels, sizeof(pixels));
  canvas = canvas_for(pixels, false);
  canvas.depth = depth;
  h2_garden_guard_render_scene(&canvas, &game);
  unsigned in_front = 0;
  for (size_t i = 0; i < 480u * 240u; ++i)
    if (depth[i] == 5u && pixels[i] != static_pixels[i]) ++in_front;
  CHECK(in_front > 0u);

  /* Drag outlines are moving-scene content; their coverage disappears on
   * release so the previous frame's bands can be restored. */
  game.pests[0].active = false;
  game.hover_lane = 0;
  game.hover_col = 8;
  check_coverage(H2_GARDEN_GUARD_PART_FIELD, true);
  game.hover_lane = game.hover_col = -1;
  canvas = canvas_for(NULL, true);
  h2_garden_guard_render_scene(&canvas, &game);
  CHECK(canvas.bands == 0u);
}

/* Pests entering from the right are clipped away from the toolbar. */
static void test_portrait_clip(void) {
  init(480, 800);
  const h2_garden_guard_layout_t* l = &game.layout;
  CHECK(l->portrait);
  game.pests[0] =
      (h2_garden_guard_pest_t){.active = true,
                               .kind = H2_GARDEN_GUARD_PEST_BEETLE,
                               .lane = 2u,
                               .x = (float)H2_GARDEN_GUARD_COLS + 0.45f,
                               .hp = 190};
  h2_garden_guard_canvas_t canvas = canvas_for(pixels, false);
  h2_garden_guard_render_scene(&canvas, &game);
  CHECK(canvas.bands != 0u);
  const size_t bar_rows = (size_t)l->bar_h;
  CHECK(memcmp(pixels, background, bar_rows * 480u * sizeof(uint16_t)) == 0);
  const int32_t toolbar_bands = (int32_t)l->bar_h / H2_GARDEN_GUARD_BAND_ROWS;
  CHECK((canvas.bands & (((uint64_t)1 << toolbar_bands) - 1u)) == 0u);
  /* The title and toolbar fit the portrait canvas. */
  check_coverage(H2_GARDEN_GUARD_PART_FIELD, false);
  check_coverage(H2_GARDEN_GUARD_PART_TOOLBAR, false);
}

static void test_small_extent(void) {
  init(160, 160);
  check_coverage(H2_GARDEN_GUARD_PART_FIELD, false);
  check_coverage(H2_GARDEN_GUARD_PART_TOOLBAR, false);
  start_playing();
  game.plants[0][0] = (h2_garden_guard_plant_t){
      .kind = H2_GARDEN_GUARD_PLANT_SUNFLOWER, .hp = 300.0f};
  check_coverage(H2_GARDEN_GUARD_PART_FIELD, false);
}

int main(void) {
  test_helpers();
  test_background();
  test_static_parts();
  test_scene();
  test_depth_and_dirty_coverage();
  test_portrait_clip();
  test_small_extent();
  puts("garden guard render tests passed");
  return 0;
}
