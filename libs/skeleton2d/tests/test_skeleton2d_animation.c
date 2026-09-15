#include "h2_skeleton2d.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define PI 3.14159265358979323846
static void angle_case(double first, double last, int shortest, int linear,
                       int64_t time, double expected) {
  h2_skeleton2d_bone_t bone = {SIZE_MAX, {12, 3, 0, 1, 1}};
  h2_skeleton2d_key_t keys[] = {{0, first}, {1000000, last}};
  h2_skeleton2d_track_t track = {0, 2, linear, shortest, keys, 2};
  h2_skeleton2d_clip_t clip = {1000000, &track, 1};
  h2_skeleton2d_config_t cfg = {&bone, 1, NULL, 0, &clip, 1};
  size_t n;
  assert(!h2_skeleton2d_definition_size(&cfg, &n));
  void *dm = malloc(n);
  h2_skeleton2d_definition_t *d;
  assert(!h2_skeleton2d_definition_init(dm, n, &cfg, &d));
  assert(!h2_skeleton2d_instance_size(d, &n));
  void *am = malloc(n);
  h2_skeleton2d_t *a;
  assert(!h2_skeleton2d_instance_init(am, n, d, &a));
  assert(!h2_skeleton2d_sample(a, 0, time, H2_SKELETON2D_CLAMP,
                               H2_SKELETON2D_CURRENT));
  double root[] = {1, 0, 0, 1, 0, 0};
  assert(!h2_skeleton2d_evaluate(a, root));
  h2_skeleton2d_view_t v;
  assert(!h2_skeleton2d_view(a, &v));
  assert(fabs(v.matrices[0] - cos(expected)) < 1e-9 &&
         fabs(v.matrices[1] - sin(expected)) < 1e-9);
  assert(v.matrices[4] == 12 && v.matrices[5] == 3 && v.item_count == 0);
  assert(!h2_skeleton2d_sample(a, 0, 0, H2_SKELETON2D_CLAMP, H2_SKELETON2D_A));
  assert(!h2_skeleton2d_sample(a, 0, 1000000, H2_SKELETON2D_CLAMP,
                               H2_SKELETON2D_B));
  for (int i = 0; i < 2; i++) {
    assert(!h2_skeleton2d_blend(a, i));
    assert(!h2_skeleton2d_evaluate(a, root));
    assert(fabs(v.matrices[1] - sin(i ? last : first)) < 1e-9);
  }
  free(am);
  free(dm);
}
int main(void) {
  angle_case(170 * PI / 180, -170 * PI / 180, 1, 1, 500000, PI);
  angle_case(170 * PI / 180, -170 * PI / 180, 0, 1, 500000, 0);
  angle_case(0, PI, 1, 1, 500000, -PI / 2);
  angle_case(0, 4 * PI, 0, 1, 250000, PI);
  angle_case(.2, 1, 1, 0, 999999, .2);
  angle_case(.2, 1, 1, 0, 1000000, 1);
  angle_case(.2, 1, 1, 1, INT64_MIN, .2);
  angle_case(.2, 1, 1, 1, INT64_MAX, 1);
  h2_skeleton2d_bone_t bones[129];
  for (size_t i = 0; i < 129; i++)
    bones[i] = (h2_skeleton2d_bone_t){i ? i - 1 : SIZE_MAX, {0, 0, 0, 1, 1}};
  h2_skeleton2d_config_t c = {bones, 128, NULL, 0, NULL, 0};
  size_t bytes;
  assert(!h2_skeleton2d_definition_size(&c, &bytes));
  c.bone_count = 129;
  assert(h2_skeleton2d_definition_size(&c, &bytes) == H2_PAL_ERR_INVALID_ARG &&
         bytes == 0);
  c.bone_count = 0;
  assert(h2_skeleton2d_definition_size(&c, &bytes) == H2_PAL_ERR_INVALID_ARG);
  c.bone_count = 128;
  bones[127].local.sx = INFINITY;
  assert(h2_skeleton2d_definition_size(&c, &bytes) == H2_PAL_ERR_INVALID_ARG);
  puts("skeleton2d animation edges: PASS");
  return 0;
}
