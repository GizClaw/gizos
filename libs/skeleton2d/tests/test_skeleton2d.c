#include "h2_skeleton2d.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OK(x) assert((x) == H2_PAL_OK)
static const double identity[] = {1, 0, 0, 1, 0, 0};
static void near(double a, double b) { assert(fabs(a - b) < 1e-9); }
static void overlapping_storage_cases(void) {
  h2_skeleton2d_bone_t bone = {SIZE_MAX, {0, 0, 0, 1, 1}};
  h2_skeleton2d_part_t part = {0, {0, 0, 0, 1, 1}, 0, 0, 1};
  h2_skeleton2d_key_t key = {0, 1};
  h2_skeleton2d_track_t track = {0, 0, 1, 0, &key, 1};
  h2_skeleton2d_clip_t clip = {1, &track, 1};
  h2_skeleton2d_config_t original = {&bone, 1, &part, 1, &clip, 1};
  size_t bytes;
  OK(h2_skeleton2d_definition_size(&original, &bytes));
  unsigned char *mem = malloc(bytes), *snapshot = malloc(bytes);
  assert(mem && snapshot);
  for (int which = 0; which < 6; ++which) {
    h2_skeleton2d_config_t cfg = original;
    h2_skeleton2d_clip_t cl = clip;
    h2_skeleton2d_track_t tr = track;
    const h2_skeleton2d_config_t *input = &cfg;
    memset(mem, 0, bytes);
    switch (which) {
    case 0: memcpy(mem, &cfg, sizeof(cfg)); input = (void *)mem; break;
    case 1: memcpy(mem, &bone, sizeof(bone)); cfg.bones = (void *)mem; break;
    case 2: memcpy(mem, &part, sizeof(part)); cfg.parts = (void *)mem; break;
    case 3: memcpy(mem, &clip, sizeof(clip)); cfg.clips = (void *)mem; break;
    case 4:
      memcpy(mem, &track, sizeof(track)); cl.tracks = (void *)mem;
      cfg.clips = &cl; break;
    default:
      memcpy(mem, &key, sizeof(key)); tr.keys = (void *)mem;
      cl.tracks = &tr; cfg.clips = &cl; break;
    }
    memcpy(snapshot, mem, bytes);
    h2_skeleton2d_definition_t *out = NULL;
    assert(h2_skeleton2d_definition_init(mem, bytes, input, &out) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(!out && !memcmp(mem, snapshot, bytes));
  }
  h2_skeleton2d_definition_t *definition;
  OK(h2_skeleton2d_definition_init(mem, bytes, &original, &definition));
  size_t instance_bytes;
  OK(h2_skeleton2d_instance_size(definition, &instance_bytes));
  memcpy(snapshot, mem, bytes);
  h2_skeleton2d_t *actor = NULL;
  assert(h2_skeleton2d_instance_init(mem, instance_bytes, definition, &actor) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(!actor && !memcmp(mem, snapshot, bytes));
  h2_skeleton2d_definition_deinit(definition);
  free(snapshot);
  free(mem);
}
static void capacity_and_order_cases(void) {
  h2_skeleton2d_bone_t bones[128];
  h2_skeleton2d_part_t parts[257];
  h2_skeleton2d_key_t keys[257];
  h2_skeleton2d_track_t tracks[65];
  for (size_t i = 0; i < 128; ++i)
    bones[i] = (h2_skeleton2d_bone_t){i ? i - 1 : SIZE_MAX, {0, 0, 0, 1, 1}};
  for (size_t i = 0; i < 257; ++i) {
    parts[i] = (h2_skeleton2d_part_t){
        i % 128, {1, 2, 0, 1, 1}, (uint32_t)i, (int32_t)(i % 7) - 3, 1};
    keys[i] = (h2_skeleton2d_key_t){(int64_t)i, 0};
  }
  for (size_t i = 0; i < 65; ++i)
    tracks[i] = (h2_skeleton2d_track_t){i / 5, (unsigned)(i % 5), 1, 0,
                                        keys,  i == 64 ? 1 : 256};
  h2_skeleton2d_clip_t clip = {256, tracks, 64};
  h2_skeleton2d_config_t cfg = {bones, 128, parts, 256, &clip, 1};
  size_t bytes;
  OK(h2_skeleton2d_definition_size(&cfg, &bytes));
  cfg.part_count = 257;
  assert(h2_skeleton2d_definition_size(&cfg, &bytes) ==
             H2_PAL_ERR_INVALID_ARG &&
         !bytes);
  cfg.part_count = 256;
  clip.track_count = 65;
  assert(h2_skeleton2d_definition_size(&cfg, &bytes) ==
             H2_PAL_ERR_INVALID_ARG &&
         !bytes);
  clip.track_count = 64;
  tracks[0].key_count = 257;
  assert(h2_skeleton2d_definition_size(&cfg, &bytes) == H2_PAL_ERR_INVALID_ARG);
  tracks[0].key_count = 256;
  tracks[1].bone = tracks[0].bone;
  tracks[1].channel = tracks[0].channel;
  assert(h2_skeleton2d_definition_size(&cfg, &bytes) == H2_PAL_ERR_INVALID_ARG);
  tracks[1].channel = 1;
  clip.track_count = 641;
  assert(h2_skeleton2d_definition_size(&cfg, &bytes) == H2_PAL_ERR_INVALID_ARG);
  clip.track_count = 64;
  cfg.clip_count = 33;
  assert(h2_skeleton2d_definition_size(&cfg, &bytes) == H2_PAL_ERR_INVALID_ARG);
  cfg.clip_count = 1;
  cfg.part_count = SIZE_MAX;
  assert(h2_skeleton2d_definition_size(&cfg, &bytes) == H2_PAL_ERR_INVALID_ARG);
  cfg.part_count = 256;
  OK(h2_skeleton2d_definition_size(&cfg, &bytes));
  unsigned char *dm = malloc(bytes + 1);
  assert(dm);
  h2_skeleton2d_definition_t *definition = NULL;
  assert(h2_skeleton2d_definition_init(dm + 1, bytes, &cfg, &definition) ==
             H2_PAL_ERR_INVALID_ARG &&
         !definition);
  OK(h2_skeleton2d_definition_init(dm, bytes, &cfg, &definition));
  OK(h2_skeleton2d_instance_size(definition, &bytes));
  void *storage[2] = {malloc(bytes), malloc(bytes)};
  h2_skeleton2d_t *actors[2];
  assert(storage[0] && storage[1]);
  assert(h2_skeleton2d_instance_init(storage[0], bytes - 1, definition,
                                     &actors[0]) == H2_PAL_ERR_NO_SPACE &&
         !actors[0]);
  for (size_t i = 0; i < 2; ++i) {
    OK(h2_skeleton2d_instance_init(storage[i], bytes, definition, &actors[i]));
    OK(h2_skeleton2d_evaluate(actors[i], identity));
  }
  h2_skeleton2d_part_state_t states[256];
  for (size_t i = 0; i < 256; ++i)
    states[i] = (h2_skeleton2d_part_state_t){
        i, (uint32_t)(256 - i), (int32_t)((i * 17) % 19) - 9, i % 3 != 0};
  OK(h2_skeleton2d_set_parts(actors[0], states, 256));
  OK(h2_skeleton2d_evaluate(actors[0], identity));
  h2_skeleton2d_view_t view, untouched;
  OK(h2_skeleton2d_view(actors[0], &view));
  OK(h2_skeleton2d_view(actors[1], &untouched));
  assert(view.item_count == 170 && untouched.item_count == 256);
  for (size_t i = 1; i < view.item_count; ++i) {
    assert(view.items[i - 1].layer <= view.items[i].layer);
    if (view.items[i - 1].layer == view.items[i].layer)
      assert(view.items[i - 1].part < view.items[i].part);
  }
  h2_skeleton2d_draw_item_t snapshot[256];
  memcpy(snapshot, view.items, view.item_count * sizeof(*snapshot));
  states[1].part = states[0].part;
  assert(h2_skeleton2d_set_parts(actors[0], states, 2) ==
         H2_PAL_ERR_INVALID_ARG);
  OK(h2_skeleton2d_evaluate(actors[0], identity));
  for (size_t i = 0; i < view.item_count; ++i) {
    assert(view.items[i].part == snapshot[i].part);
    assert(view.items[i].layer == snapshot[i].layer);
  }
  /* Cached ordering must still publish changed resource IDs. */
  h2_skeleton2d_part_state_t resource_change = {1, 123456, states[1].layer, 1};
  OK(h2_skeleton2d_set_parts(actors[0], &resource_change, 1));
  OK(h2_skeleton2d_evaluate(actors[0], identity));
  for (size_t i = 0; i < view.item_count; ++i)
    if (view.items[i].part == 1)
      assert(view.items[i].resource == 123456);
  h2_skeleton2d_local_t move = {0, {7, 9, 0, 1, 1}};
  OK(h2_skeleton2d_set_local(actors[0], &move, 1));
  OK(h2_skeleton2d_evaluate(actors[0], identity));
  near(view.matrices[4], 7);
  near(untouched.matrices[4], 0);
  for (size_t i = 0; i < 2; ++i) {
    h2_skeleton2d_instance_deinit(actors[i]);
    free(storage[i]);
  }
  h2_skeleton2d_definition_deinit(definition);
  assert(h2_skeleton2d_instance_size(definition, &bytes) ==
             H2_PAL_ERR_INVALID_STATE &&
         !bytes);
  h2_skeleton2d_definition_deinit(definition);
  free(dm);
}
int main(void) {
  overlapping_storage_cases();
  capacity_and_order_cases();
  h2_skeleton2d_bone_t bones[] = {{SIZE_MAX, {10, 20, 0, 2, 1}},
                                  {0, {5, 0, 1.5707963267948966, 1, 1}}};
  h2_skeleton2d_part_t parts[] = {{0, {0, 0, 0, 1, 1}, 1, 0, 1},
                                  {1, {0, 0, 0, 1, 1}, 2, 0, 1}};
  h2_skeleton2d_key_t keys[] = {{0, 0}, {1000000, 10}};
  h2_skeleton2d_track_t tracks[] = {{0, 0, 1, 0, keys, 2}};
  h2_skeleton2d_clip_t clips[] = {{1000000, tracks, 1}};
  h2_skeleton2d_config_t cfg = {bones, 2, parts, 2, clips, 1};
  size_t bytes;
  OK(h2_skeleton2d_definition_size(&cfg, &bytes));
  void *dm = malloc(bytes);
  h2_skeleton2d_definition_t *d;
  assert(h2_skeleton2d_definition_init(dm, bytes - 1, &cfg, &d) ==
             H2_PAL_ERR_NO_SPACE &&
         d == NULL);
  OK(h2_skeleton2d_definition_init(dm, bytes, &cfg, &d));
  keys[1].value = 99;
  OK(h2_skeleton2d_instance_size(d, &bytes));
  void *am = malloc(bytes);
  h2_skeleton2d_t *a;
  OK(h2_skeleton2d_instance_init(am, bytes, d, &a));
  h2_skeleton2d_view_t v;
  assert(h2_skeleton2d_view(a, &v) == H2_PAL_ERR_INVALID_STATE &&
         v.items == NULL);
  OK(h2_skeleton2d_evaluate(a, identity));
  OK(h2_skeleton2d_view(a, &v));
  near(v.matrices[10], 20);
  near(v.matrices[11], 20);
  near(v.matrices[6], 0);
  near(v.matrices[7], 1);
  near(v.matrices[8], -2);
  assert(v.items[0].part == 0 && v.items[1].part == 1);
  OK(h2_skeleton2d_sample(a, 0, 500000, H2_SKELETON2D_REPEAT,
                          H2_SKELETON2D_CURRENT));
  OK(h2_skeleton2d_evaluate(a, identity));
  near(v.matrices[4], 5);
  OK(h2_skeleton2d_sample(a, 0, -250000, H2_SKELETON2D_REPEAT,
                          H2_SKELETON2D_CURRENT));
  OK(h2_skeleton2d_evaluate(a, identity));
  near(v.matrices[4], 7.5);
  OK(h2_skeleton2d_sample(a, 0, 1000000, H2_SKELETON2D_REPEAT,
                          H2_SKELETON2D_CURRENT));
  OK(h2_skeleton2d_evaluate(a, identity));
  near(v.matrices[4], 0);
  OK(h2_skeleton2d_sample(a, 0, 1000000, H2_SKELETON2D_CLAMP, H2_SKELETON2D_B));
  OK(h2_skeleton2d_sample(a, 0, 0, H2_SKELETON2D_CLAMP, H2_SKELETON2D_A));
  OK(h2_skeleton2d_blend(a, .25));
  OK(h2_skeleton2d_evaluate(a, identity));
  near(v.matrices[4], 2.5);
  h2_skeleton2d_part_state_t ps[] = {{0, 1, 2, 1}, {1, 2, -1, 1}};
  OK(h2_skeleton2d_set_parts(a, ps, 2));
  OK(h2_skeleton2d_evaluate(a, identity));
  assert(v.items[0].part == 1);
  double old[12];
  memcpy(old, v.matrices, sizeof(old));
  double bad[] = {1e6, 0, 0, 1e6, 0, 0};
  assert(h2_skeleton2d_evaluate(a, bad) == H2_PAL_ERR_INVALID_ARG);
  assert(!memcmp(old, v.matrices, sizeof(old)));
  h2_skeleton2d_local_t override[] = {{0, {0, 0, 0, 0, -1}},
                                      {0, {NAN, 0, 0, 1, 1}}};
  assert(h2_skeleton2d_set_local(a, override, 2) == H2_PAL_ERR_INVALID_ARG);
  OK(h2_skeleton2d_evaluate(a, identity));
  assert(!memcmp(old, v.matrices, sizeof(old)));
  OK(h2_skeleton2d_set_local(a, override, 1));
  OK(h2_skeleton2d_evaluate(a, identity));
  near(v.matrices[0], 0);
  near(v.matrices[3], -1);
  for (int i = 0; i < 10000; i++) {
    OK(h2_skeleton2d_sample(a, 0, i * 33333, H2_SKELETON2D_REPEAT,
                            H2_SKELETON2D_CURRENT));
    OK(h2_skeleton2d_evaluate(a, identity));
  }
  const int64_t seeks[] = {INT64_MIN, -1,      0,       1,
                           999999,    1000000, 1000001, INT64_MAX,
                           250000,    -250000, 500000};
  for (size_t i = 0; i < sizeof(seeks) / sizeof(seeks[0]); ++i) {
    int64_t offset = seeks[i] % 1000000;
    if (offset < 0)
      offset += 1000000;
    OK(h2_skeleton2d_sample(a, 0, seeks[i], H2_SKELETON2D_REPEAT,
                            H2_SKELETON2D_CURRENT));
    OK(h2_skeleton2d_evaluate(a, identity));
    near(v.matrices[4], (double)offset / 100000.0);
  }
  assert(h2_skeleton2d_sample(a, 0, 0, H2_SKELETON2D_REPEAT,
                              (h2_skeleton2d_pose_slot_t)-1) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_skeleton2d_sample(a, 0, 0, H2_SKELETON2D_REPEAT,
                              (h2_skeleton2d_pose_slot_t)3) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_skeleton2d_set_local(a, NULL, 1) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_skeleton2d_set_parts(a, NULL, 1) == H2_PAL_ERR_INVALID_ARG);
  OK(h2_skeleton2d_set_local(a, NULL, 0));
  OK(h2_skeleton2d_set_parts(a, NULL, 0));
  h2_skeleton2d_instance_deinit(a);
  h2_skeleton2d_instance_deinit(a);
  assert(h2_skeleton2d_evaluate(a, identity) == H2_PAL_ERR_INVALID_STATE);
  free(am);
  h2_skeleton2d_definition_deinit(d);
  free(dm);
  bones[1].parent = 1;
  assert(h2_skeleton2d_definition_size(&cfg, &bytes) ==
             H2_PAL_ERR_INVALID_ARG &&
         bytes == 0);
  bones[1].parent = 0;
  keys[1].time_us = 0;
  assert(h2_skeleton2d_definition_size(&cfg, &bytes) == H2_PAL_ERR_INVALID_ARG);
  puts("skeleton2d core: PASS");
  return 0;
}
