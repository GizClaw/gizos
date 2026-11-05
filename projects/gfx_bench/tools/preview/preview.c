/* Off-screen preview: writes one PPM per backend/look for visual comparison. */
#include "h2_gfx_scene.h"

#include <stdio.h>
#include <stdlib.h>

enum { WIDTH = 480, HEIGHT = 480 };

static uint16_t color[WIDTH * HEIGHT];
static uint16_t depth[WIDTH * HEIGHT];
static uint16_t sky[HEIGHT];

static uint16_t rgb(int red, int green, int blue) {
  return (uint16_t)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
}

static int save(const char* directory, const char* backend, int look,
                float yaw) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s-%d-%d.ppm", directory, backend, look,
           (int)(yaw * 100.0f));
  FILE* file = fopen(path, "wb");
  if (!file) return -1;
  fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
  for (int i = 0; i < WIDTH * HEIGHT; ++i) {
    const uint16_t c = color[i];
    const unsigned char pixel[3] = {(unsigned char)((c >> 11) * 255 / 31),
                                    (unsigned char)(((c >> 5) & 63) * 255 / 63),
                                    (unsigned char)((c & 31) * 255 / 31)};
    fwrite(pixel, 1, 3, file);
  }
  return fclose(file);
}

static void* allocate(void* user, size_t size) {
  (void)user;
  return malloc(size);
}
static void release(void* user, void* ptr) {
  (void)user;
  free(ptr);
}
static const h2_pal_mem_vtable_t memory_ops = {.alloc = allocate,
                                               .free = release};
static const h2_pal_mem_api_t memory = {.vtable = &memory_ops};

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  for (int y = 0; y < HEIGHT; ++y)
    sky[y] = rgb(14 + y / 12, 18 + y / 10, 34 + y / 8);
  const h2_gfx_target_t target = {color, depth, WIDTH, HEIGHT, sky, &memory};
  const h2_gfx_backend_t* backends[] = {&h2_gfx_backend_tgx,
                                        &h2_gfx_backend_jet};
  const float yaws[] = {0.7f, 3.6f};
  for (int b = 0; b < 2; ++b) {
    if (backends[b]->init(&target) != 0) return 1;
    for (int look = 0; look < H2_GFX_LOOK_COUNT; ++look)
      for (int y = 0; y < 2; ++y) {
        const h2_gfx_view_t view = {0.0f, 0.0f, yaws[y], (h2_gfx_look_t)look};
        backends[b]->render(&view);
        if (save(argv[1], backends[b]->name, look, yaws[y]) != 0) return 1;
      }
    backends[b]->deinit();
  }
  return 0;
}
