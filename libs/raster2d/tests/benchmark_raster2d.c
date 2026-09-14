#include "h2_raster2d.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SAMPLES 128u
#define REPEATS 16u
#define MAX_RECTS 4096u
static uint16_t pixels[240u * 240u];
static h2_raster2d_rect_t rects[MAX_RECTS];
static uint16_t a[MAX_RECTS], b[MAX_RECTS], output[MAX_RECTS];

static int compare(const void *a_value, const void *b_value) {
  double a_time = *(const double *)a_value, b_time = *(const double *)b_value;
  return (a_time > b_time) - (a_time < b_time);
}

static void measure(size_t count, const h2_raster2d_clip_t *clip, int blend) {
  double samples[SAMPLES];
  const h2_raster2d_surface_t surface = {pixels, 240u * 240u, 240, 240, 240};
  for (unsigned sample = 0; sample < SAMPLES + 8u; ++sample) {
    clock_t start = clock();
    assert(start != (clock_t)-1);
    for (unsigned repeat = 0; repeat < REPEATS; ++repeat) {
      h2_pal_result_t result =
          blend ? h2_raster2d_palette_blend(a, b, count, sample % 257u, output,
                                            count)
                : h2_raster2d_draw_rects(&surface, rects, count, output, count,
                                         clip);
      if (result != H2_PAL_OK)
        abort();
    }
    clock_t end = clock();
    assert(end != (clock_t)-1 && end >= start);
    if (sample >= 8u)
      samples[sample - 8u] =
          (double)(end - start) * 1e6 / CLOCKS_PER_SEC / REPEATS;
  }
  qsort(samples, SAMPLES, sizeof(samples[0]), compare);
  printf("core count=%zu clip=%s phase=%s p50_us=%.3f p95_us=%.3f "
         "calls_per_sample=%u allocations=0\n",
         count, clip ? "partial" : "full", blend ? "palette" : "replay",
         samples[SAMPLES / 2], samples[(SAMPLES * 95u) / 100u], REPEATS);
}

int main(void) {
  puts("raster2d host CPU-time benchmark; warmup=8 samples=128 repeats=16; no "
       "Display/Lua costs");
  printf("preallocated_storage_bytes=%zu (includes largest workload); core "
         "heap allocations=0 (harness excluded)\n",
         sizeof(pixels) + sizeof(rects) + sizeof(a) + sizeof(b) +
             sizeof(output));
  const size_t counts[] = {96, 1536, 4096};
  for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); ++c) {
    size_t count = counts[c], columns = c == 0 ? 12 : c == 1 ? 48 : 64;
    size_t rows = count / columns;
    clock_t start = clock();
    for (size_t i = 0; i < count; ++i) {
      size_t x = i % columns, y = i / columns;
      rects[i] = (h2_raster2d_rect_t){
          (int32_t)(x * 240 / columns), (int32_t)(y * 240 / rows),
          (uint32_t)((x + 1) * 240 / columns - x * 240 / columns),
          (uint32_t)((y + 1) * 240 / rows - y * 240 / rows), (uint32_t)i};
      a[i] = (uint16_t)(i * 73u);
      b[i] = (uint16_t)(i * 151u);
      output[i] = a[i];
    }
    printf("core count=%zu initialization_cpu_us=%.3f geometry_bytes=%zu "
           "palette_bytes=%zu\n",
           count, (double)(clock() - start) * 1e6 / CLOCKS_PER_SEC,
           count * sizeof(*rects), 3 * count * sizeof(*a));
    measure(count, NULL, 1);
    measure(count, NULL, 0);
    /* A clipped/overlapping workload has the same bounded inputs. */
    for (size_t i = 0; i < count; i += 3)
      rects[i].x -= 20;
    const h2_raster2d_clip_t clip = {30, 30, 210, 210};
    measure(count, &clip, 0);
  }
  return 0;
}
