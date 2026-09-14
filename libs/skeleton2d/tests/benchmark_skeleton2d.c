#define _POSIX_C_SOURCE 200809L
#include "h2_skeleton2d.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}
static int compare(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}
int main(void) {
  for (size_t nb = 16; nb <= 128; nb *= 2) {
    size_t np = nb * 3 / 2;
    h2_skeleton2d_bone_t bones[128];
    h2_skeleton2d_part_t parts[192];
    h2_skeleton2d_track_t tracks[128];
    h2_skeleton2d_key_t keys[] = {{0, -.1}, {1000000, .1}};
    for (size_t i = 0; i < nb; i++) {
      bones[i] =
          (h2_skeleton2d_bone_t){i ? i - 1 : SIZE_MAX, {i ? 2 : 0, 0, 0, 1, 1}};
      tracks[i] = (h2_skeleton2d_track_t){i, 2, 1, 1, keys, 2};
    }
    for (size_t i = 0; i < np; i++)
      parts[i] = (h2_skeleton2d_part_t){i % nb, {0, 0, 0, 1, 1}, 1, (int)i, 1};
    h2_skeleton2d_clip_t clip = {1000000, tracks, nb};
    h2_skeleton2d_config_t c = {bones, nb, parts, np, &clip, 1};
    size_t db, ab;
    assert(!h2_skeleton2d_definition_size(&c, &db));
    void *dm = malloc(db);
    h2_skeleton2d_definition_t *d;
    assert(!h2_skeleton2d_definition_init(dm, db, &c, &d));
    assert(!h2_skeleton2d_instance_size(d, &ab));
    void *am = malloc(ab);
    h2_skeleton2d_t *a;
    assert(!h2_skeleton2d_instance_init(am, ab, d, &a));
    const double root[] = {1, 0, 0, 1, 0, 0};
    for (int run = 0; run < 3; run++) {
      double samples[3000];
      for (int i = -300; i < 3000; i++) {
        double t = now();
        assert(!h2_skeleton2d_sample(a, 0, i * 33333, H2_SKELETON2D_REPEAT,
                                     H2_SKELETON2D_A));
        assert(!h2_skeleton2d_sample(a, 0, i * 33333 + 200000,
                                     H2_SKELETON2D_REPEAT, H2_SKELETON2D_B));
        assert(!h2_skeleton2d_blend(a, .5));
        assert(!h2_skeleton2d_evaluate(a, root));
        if (i >= 0)
          samples[i] = now() - t;
      }
      qsort(samples, 3000, sizeof(double), compare);
      printf("CORE bones=%zu parts=%zu run=%d definition_bytes=%zu "
             "instance_bytes=%zu p50_us=%.3f p95_us=%.3f p99_us=%.3f "
             "max_us=%.3f\n",
             nb, np, run + 1, db, ab, samples[1499], samples[2849],
             samples[2969], samples[2999]);
    }
    free(am);
    free(dm);
  }
  return 0;
}
