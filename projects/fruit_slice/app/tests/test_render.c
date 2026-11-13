#include "h2_fruit_slice_game.h"
#include "h2_fruit_slice_render.h"

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

#define MAX_W 800
#define MAX_H 480
#define SENTINEL 0x0841u

static uint16_t background[MAX_W * MAX_H];
static uint16_t pixels[MAX_W * MAX_H];
static h2_fruit_slice_game_t game;

/* Optional review artifact: Bazel collects files in this directory. */
static void write_ppm(const char* name, int32_t width, int32_t height) {
  const char* directory = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (directory == NULL || directory[0] == '\0') return;
  char path[512];
  (void)snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
  FILE* file = fopen(path, "wb");
  CHECK(file != NULL);
  fprintf(file, "P6\n%d %d\n255\n", (int)width, (int)height);
  for (int32_t i = 0; i < width * height; ++i) {
    const uint16_t c = pixels[i];
    const unsigned char rgb[3] = {(unsigned char)((c >> 11) << 3),
                                  (unsigned char)(((c >> 5) & 63u) << 2),
                                  (unsigned char)((c & 31u) << 3)};
    CHECK(fwrite(rgb, 1, 3, file) == 3u);
  }
  CHECK(fclose(file) == 0);
}

static void* test_alloc(void* user, size_t length) {
  (void)user;
  return malloc(length);
}
static void* test_realloc(void* user, void* pointer, size_t length) {
  (void)user;
  return realloc(pointer, length);
}
static void test_free(void* user, void* pointer) {
  (void)user;
  free(pointer);
}
static const h2_pal_mem_vtable_t test_vtable = {
    .alloc = test_alloc, .realloc = test_realloc, .free = test_free};
static const h2_pal_mem_api_t test_memory = {.vtable = &test_vtable};

static void* failing_alloc(void* user, size_t length) {
  size_t* remaining = user;
  if (*remaining == 0u) return NULL;
  --*remaining;
  return malloc(length);
}
static const h2_pal_mem_vtable_t failing_vtable = {
    .alloc = failing_alloc, .realloc = test_realloc, .free = test_free};

static h2_fruit_slice_sprites_t* sprites;

typedef void (*layer_fn)(h2_fruit_slice_canvas_t*,
                         const h2_fruit_slice_game_t*);

/* The paint pass may only touch bands reported by the measure pass. */
static size_t check_layer(layer_fn layer, h2_fruit_slice_sprites_t* cache,
                          int32_t width, int32_t height) {
  h2_fruit_slice_canvas_t canvas = {.pixels = NULL,
                                    .width = width,
                                    .height = height,
                                    .stride_pixels = (size_t)width,
                                    .measure = true,
                                    .sprites = cache};
  layer(&canvas, &game);
  const uint64_t measured = canvas.bands;
  for (int32_t i = 0; i < width * height; ++i) pixels[i] = SENTINEL;
  canvas.pixels = pixels;
  canvas.measure = false;
  canvas.bands = 0;
  layer(&canvas, &game);
  CHECK(canvas.bands == measured);
  size_t changed = 0;
  for (int32_t y = 0; y < height; ++y)
    for (int32_t x = 0; x < width; ++x)
      if (pixels[y * width + x] != SENTINEL) {
        ++changed;
        CHECK(measured & ((uint64_t)1 << (y / H2_FRUIT_SLICE_BAND_ROWS)));
      }
  CHECK((changed > 0u) == (measured != 0u));
  return changed;
}

static void check_scene(const char* name, int32_t width, int32_t height) {
  CHECK(check_layer(h2_fruit_slice_render_scene, sprites, width, height) > 0u);
  CHECK(check_layer(h2_fruit_slice_render_scene, NULL, width, height) > 0u);
  (void)check_layer(h2_fruit_slice_render_static, sprites, width, height);

  /* Composite all three layers for the review artifact. */
  h2_fruit_slice_canvas_t canvas = {.pixels = pixels,
                                    .width = width,
                                    .height = height,
                                    .stride_pixels = (size_t)width,
                                    .sprites = sprites};
  memcpy(pixels, background, (size_t)width * (size_t)height * 2u);
  h2_fruit_slice_render_static(&canvas, &game);
  h2_fruit_slice_render_scene(&canvas, &game);
  write_ppm(name, width, height);
}

static void test_static_key(void) {
  h2_fruit_slice_static_key_t a, b;
  h2_fruit_slice_game_init(&game, 480, 480, 3u, 5u);
  h2_fruit_slice_static_key(&game, &a);
  CHECK(a.phase == H2_FRUIT_SLICE_PHASE_TITLE && a.best == 5u && a.menu_y > 0);
  /* Score and lives are not shown on the title screen. */
  game.score = 9u;
  h2_fruit_slice_static_key(&game, &b);
  CHECK(memcmp(&a, &b, sizeof(a)) == 0);
  game.phase = H2_FRUIT_SLICE_PHASE_PLAYING;
  memset(game.fruits, 0, sizeof(game.fruits));
  h2_fruit_slice_static_key(&game, &a);
  CHECK(a.score == 9u && a.menu_y == -1);
  game.lives = 1u;
  h2_fruit_slice_static_key(&game, &b);
  CHECK(memcmp(&a, &b, sizeof(a)) != 0);
}

static void test_sprite_cache(void) {
  h2_fruit_slice_game_init(&game, 368, 448, 4u, 0u);
  /* A failed texture allocation reports failure without leaking. */
  size_t remaining = 3u;
  const h2_pal_mem_api_t failing = {.user = &remaining,
                                    .vtable = &failing_vtable};
  h2_fruit_slice_sprites_t* limited = h2_fruit_slice_sprites_create(&failing);
  CHECK(limited != NULL);
  CHECK(!h2_fruit_slice_sprites_prepare(limited, &game));
  h2_fruit_slice_sprites_destroy(limited);
  remaining = 0u;
  CHECK(h2_fruit_slice_sprites_create(&failing) == NULL);
  h2_fruit_slice_sprites_destroy(NULL);

  /* Sizes beyond capacity fall back to flat drawing. */
  h2_fruit_slice_sprites_t* small = h2_fruit_slice_sprites_create(&test_memory);
  CHECK(small != NULL);
  game.phase = H2_FRUIT_SLICE_PHASE_PLAYING;
  memset(game.fruits, 0, sizeof(game.fruits));
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i) {
    h2_fruit_slice_fruit_t* f = &game.fruits[i];
    f->active = true;
    f->kind = (uint8_t)(i % H2_FRUIT_SLICE_KIND_COUNT);
    f->radius = 10.0f + 2.5f * (float)i;
    f->body.x = 20.0f + 20.0f * (float)i;
    f->body.y = 200.0f;
  }
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i) {
    h2_fruit_slice_half_t* h = &game.halves[i];
    h->active = true;
    h->kind = (uint8_t)(i % H2_FRUIT_SLICE_FRUIT_KINDS);
    h->radius = 11.0f + 2.5f * (float)i;
    h->body.x = 20.0f + 20.0f * (float)i;
    h->body.y = 320.0f;
  }
  CHECK(check_layer(h2_fruit_slice_render_scene, small, 368, 448) > 0u);
  CHECK(check_layer(h2_fruit_slice_render_scene, small, 368, 448) > 0u);
  h2_fruit_slice_sprites_destroy(small);
  memset(game.halves, 0, sizeof(game.halves));
}

static void scene_playing(void) {
  game.phase = H2_FRUIT_SLICE_PHASE_PLAYING;
  game.score = 27;
  game.best = 41;
  game.lives = 2;
  memset(game.fruits, 0, sizeof(game.fruits));
  const float w = (float)game.width, h = (float)game.height;
  for (unsigned k = 0; k < H2_FRUIT_SLICE_KIND_COUNT; ++k) {
    h2_fruit_slice_fruit_t* f = &game.fruits[k];
    f->active = true;
    f->kind = (uint8_t)k;
    f->radius = h2_fruit_slice_game_radius(&game, (h2_fruit_slice_kind_t)k);
    f->body.x = w * (0.12f + 0.155f * (float)k);
    f->body.y = h * (k % 2u ? 0.36f : 0.52f);
    f->body.angle = 0.4f * (float)k;
  }
  for (unsigned k = 0; k < H2_FRUIT_SLICE_FRUIT_KINDS; ++k) {
    for (unsigned side = 0; side < 2u; ++side) {
      h2_fruit_slice_half_t* half = &game.halves[k * 2u + side];
      half->active = true;
      half->kind = (uint8_t)k;
      half->radius =
          h2_fruit_slice_game_radius(&game, (h2_fruit_slice_kind_t)k);
      half->body.x = w * (0.12f + 0.19f * (float)k) + (side ? 14.0f : -14.0f);
      half->body.y = h * 0.80f;
      half->body.angle = (side ? 0.0f : H2_FRUIT_SLICE_PI) + 0.3f;
    }
  }
  for (unsigned i = 0; i < 24u; ++i) {
    h2_fruit_slice_particle_t* p = &game.particles[i];
    *p = (h2_fruit_slice_particle_t){
        .active = true,
        .x = w * 0.5f + (float)(i * 7u % 90u),
        .y = h * 0.66f - (float)(i * 11u % 60u),
        .size = 4.0f,
        .color = h2_fruit_slice_game_juice((h2_fruit_slice_kind_t)(i % 5u)),
        .life_ms = 500u,
        .age_ms = (uint16_t)(i * 15u)};
  }
  game.splats[0] = (h2_fruit_slice_splat_t){
      .active = true,
      .x = w * 0.30f,
      .y = h * 0.66f,
      .radius = 50.0f,
      .color = h2_fruit_slice_game_juice(H2_FRUIT_SLICE_WATERMELON),
      .life_ms = 900u,
      .age_ms = 200u};
  game.popups[0] =
      (h2_fruit_slice_popup_t){.active = true,
                               .x = w * 0.5f,
                               .y = h * 0.20f,
                               .color = H2_FRUIT_SLICE_RGB(255, 214, 64),
                               .life_ms = 900u,
                               .text = "COMBO +3"};
  game.misses[0] = (h2_fruit_slice_miss_t){.active = true, .x = w * 0.2f};
  game.time_ms = 10000u;
  game.trail_count = 0;
  for (unsigned i = 0; i < 10u; ++i)
    game.trail[game.trail_count++] = (h2_fruit_slice_trail_point_t){
        .x = w * 0.15f + (float)i * w * 0.07f,
        .y = h * 0.30f + (float)(i * i) * 1.8f,
        .time_ms = game.time_ms - 100u + i * 10u};
}

static void render_size(int32_t width, int32_t height) {
  h2_fruit_slice_canvas_t canvas = {.pixels = background,
                                    .width = width,
                                    .height = height,
                                    .stride_pixels = (size_t)width};
  h2_fruit_slice_render_background(&canvas);
  CHECK(canvas.bands == h2_fruit_slice_all_bands(height));
  CHECK(background[0] != background[width * height / 2 + width / 2]);

  char name[64];
  h2_fruit_slice_game_init(&game, width, height, 11u, 12u);
  CHECK(h2_fruit_slice_sprites_prepare(sprites, &game));
  h2_fruit_slice_game_step(&game, 100u);
  (void)snprintf(name, sizeof(name), "title_%dx%d", (int)width, (int)height);
  check_scene(name, width, height);

  scene_playing();
  (void)snprintf(name, sizeof(name), "playing_%dx%d", (int)width, (int)height);
  check_scene(name, width, height);

  memset(game.fruits, 0, sizeof(game.fruits));
  game.phase = H2_FRUIT_SLICE_PHASE_GAME_OVER;
  game.phase_started_ms = game.time_ms;
  game.new_best = true;
  game.best = game.score;
  h2_fruit_slice_game_step(&game, H2_FRUIT_SLICE_RESTART_DELAY_MS);
  (void)snprintf(name, sizeof(name), "game_over_%dx%d", (int)width,
                 (int)height);
  check_scene(name, width, height);

  game.flash_ms = H2_FRUIT_SLICE_FLASH_MS / 2u;
  game.flash_x = (float)width * 0.5f;
  game.flash_y = (float)height * 0.5f;
  (void)snprintf(name, sizeof(name), "bomb_flash_%dx%d", (int)width,
                 (int)height);
  check_scene(name, width, height);
}

static void test_blast_bounds(int32_t width, int32_t height) {
  h2_fruit_slice_game_init(&game, width, height, 19u, 0u);
  game.phase = H2_FRUIT_SLICE_PHASE_PLAYING;
  memset(game.fruits, 0, sizeof(game.fruits));
  const float centers[][2] = {{(float)width * 0.5f, (float)height * 0.5f},
                              {0.0f, 0.0f},
                              {(float)width - 1.0f, (float)height - 1.0f}};
  for (unsigned center = 0; center < 3u; ++center) {
    game.flash_x = centers[center][0];
    game.flash_y = centers[center][1];
    for (uint32_t elapsed = 0; elapsed < H2_FRUIT_SLICE_FLASH_MS;
         elapsed += 100u) {
      game.flash_ms = H2_FRUIT_SLICE_FLASH_MS - elapsed;
      h2_fruit_slice_canvas_t canvas = {.width = width,
                                        .height = height,
                                        .stride_pixels = (size_t)width,
                                        .measure = true};
      h2_fruit_slice_render_scene(&canvas, &game);
      size_t submitted = 0;
      for (int band = 0; band < H2_FRUIT_SLICE_MAX_BANDS; ++band) {
        if (!(canvas.bands & (UINT64_C(1) << band))) continue;
        const int y0 = band * H2_FRUIT_SLICE_BAND_ROWS;
        const int rows = height - y0 < H2_FRUIT_SLICE_BAND_ROWS
                             ? height - y0
                             : H2_FRUIT_SLICE_BAND_ROWS;
        submitted += (size_t)(canvas.band_x1[band] - canvas.band_x0[band]) *
                     (size_t)rows;
      }
      /* The complete flash, including aligned rows, stays below a quarter
       * of the screen at every age and clips safely against both corners. */
      CHECK(submitted > 0 && submitted < (size_t)width * (size_t)height / 4u);
      CHECK(check_layer(h2_fruit_slice_render_scene, NULL, width, height) > 0);
    }
  }
  game.flash_ms = 0;
  CHECK(check_layer(h2_fruit_slice_render_scene, NULL, width, height) == 0);
}

static void test_bands(void) {
  CHECK(h2_fruit_slice_all_bands(0) == 0u);
  CHECK(h2_fruit_slice_all_bands(16) == 1u);
  CHECK(h2_fruit_slice_all_bands(17) == 3u);
  CHECK(h2_fruit_slice_all_bands(480) == ((uint64_t)1 << 30) - 1u);
  CHECK(h2_fruit_slice_all_bands(1024) == UINT64_MAX);
  CHECK(h2_fruit_slice_text_width("", 3) == 0);
  CHECK(h2_fruit_slice_text_width("AB", 2) == 22);
}

/* Off-screen and degenerate objects must clip without touching memory. */
static void test_clipping(void) {
  h2_fruit_slice_game_init(&game, 480, 480, 12u, 0u);
  game.phase = H2_FRUIT_SLICE_PHASE_PLAYING;
  memset(game.fruits, 0, sizeof(game.fruits));
  const float positions[][2] = {{-500.0f, 240.0f},
                                {240.0f, 2000.0f},
                                {-20.0f, -20.0f},
                                {500.0f, 470.0f},
                                {1e30f, -1e30f}};
  for (unsigned i = 0; i < 5u; ++i) {
    h2_fruit_slice_fruit_t* f = &game.fruits[i];
    f->active = true;
    f->kind = (uint8_t)(i % H2_FRUIT_SLICE_KIND_COUNT);
    f->radius = 40.0f;
    f->body.x = positions[i][0];
    f->body.y = positions[i][1];
  }
  check_scene("clipping", 480, 480);
}

int main(void) {
  sprites = h2_fruit_slice_sprites_create(&test_memory);
  CHECK(sprites != NULL);
  test_bands();
  test_static_key();
  test_sprite_cache();
  render_size(480, 480);
  render_size(800, 480);
  render_size(368, 448);
  test_blast_bounds(480, 480);
  test_blast_bounds(800, 480);
  test_blast_bounds(368, 448);
  test_clipping();
  h2_fruit_slice_sprites_destroy(sprites);
  puts("fruit slice render tests passed");
  return 0;
}
