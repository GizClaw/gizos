#ifndef H2_SKELETON2D_H
#define H2_SKELETON2D_H
/** @file h2_skeleton2d.h
 * @brief Bounded, allocation-free planar skeletal animation.
 * Caller owns max_align_t-aligned storage returned by size queries. Definitions
 * copy all input arrays; instances borrow a definition until deinit. All calls
 * are synchronous, single-owner, not ISR safe. Distinct instances may share an
 * immutable definition. No Lua, renderer, clock, task or SDK dependency.
 * Positions, angles, scales and matrix results are finite within +/-1e6.
 * Matrices are {a,b,c,d,tx,ty}: x'=a*x+c*y+tx, y'=b*x+d*y+ty.
 * Updates validate before publishing; failure preserves existing pose/view.
 * View pointers expire on next successful evaluate or instance deinit.
 */
#include "h2/pal/core/h2_pal_errors.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define H2_SKELETON2D_BONE_LIMIT 128u
#define H2_SKELETON2D_PART_LIMIT 256u
#define H2_SKELETON2D_CLIP_LIMIT 32u
#define H2_SKELETON2D_KEY_LIMIT 16384u
typedef struct h2_skeleton2d_definition h2_skeleton2d_definition_t;
typedef struct h2_skeleton2d h2_skeleton2d_t;
/** Local T*R*S; origin is the joint. Zero and negative scale are valid. */
typedef struct h2_skeleton2d_transform {
  double x, y, angle, sx, sy;
} h2_skeleton2d_transform_t;
typedef struct h2_skeleton2d_bone {
  size_t parent;
  h2_skeleton2d_transform_t local;
} h2_skeleton2d_bone_t;
typedef struct h2_skeleton2d_part {
  size_t bone;
  h2_skeleton2d_transform_t local;
  uint32_t resource;
  int32_t layer;
  int visible;
} h2_skeleton2d_part_t;
typedef struct h2_skeleton2d_key {
  int64_t time_us;
  double value;
} h2_skeleton2d_key_t;
/** channel: x,y,angle,sx,sy = 0..4. linear=0 step, 1 linear.
 * shortest applies only to angle; otherwise interpolate unwrapped values. */
typedef struct h2_skeleton2d_track {
  size_t bone;
  unsigned channel;
  int linear, shortest;
  const h2_skeleton2d_key_t *keys;
  size_t key_count;
} h2_skeleton2d_track_t;
typedef struct h2_skeleton2d_clip {
  int64_t duration_us;
  const h2_skeleton2d_track_t *tracks;
  size_t track_count;
} h2_skeleton2d_clip_t;
/** IDs are zero-based. Exactly one root is required at bone 0, parent SIZE_MAX;
 * every later bone has parent < its own index. Parts reference valid bones.
 * Limits: 1..128 bones, 0..256 parts, 0..32 clips, 0..640 tracks per clip,
 * 1..256 keys per track and at most 16384 keys across the definition.
 * A bone/channel pair occurs at most once per clip. Duration is 1..3600000000
 * microseconds; keys lie in [0,duration], with strictly increasing times.
 * Flags must be 0 or 1. C resource/layer values are opaque uint32/int32 IDs;
 * an adapter may enforce a narrower resource range. Input must not overlap
 * the destination arena. Arrays are copied; zero-count arrays may be NULL.
 */
typedef struct h2_skeleton2d_config {
  const h2_skeleton2d_bone_t *bones;
  size_t bone_count;
  const h2_skeleton2d_part_t *parts;
  size_t part_count;
  const h2_skeleton2d_clip_t *clips;
  size_t clip_count;
} h2_skeleton2d_config_t;
typedef enum h2_skeleton2d_loop {
  H2_SKELETON2D_CLAMP,
  H2_SKELETON2D_REPEAT
} h2_skeleton2d_loop_t;
typedef enum h2_skeleton2d_pose_slot {
  H2_SKELETON2D_CURRENT,
  H2_SKELETON2D_A,
  H2_SKELETON2D_B
} h2_skeleton2d_pose_slot_t;
typedef struct h2_skeleton2d_local {
  size_t bone;
  h2_skeleton2d_transform_t local;
} h2_skeleton2d_local_t;
typedef struct h2_skeleton2d_part_state {
  size_t part;
  uint32_t resource;
  int32_t layer;
  int visible;
} h2_skeleton2d_part_state_t;
typedef struct h2_skeleton2d_draw_item {
  size_t part;
  uint32_t resource;
  int32_t layer;
  double matrix[6];
} h2_skeleton2d_draw_item_t;
typedef struct h2_skeleton2d_view {
  const double *matrices;
  size_t bone_count;
  const h2_skeleton2d_draw_item_t *items;
  size_t item_count;
} h2_skeleton2d_view_t;
/** Queries clear out_bytes on error. Init clears out handle on failure.
 * Invalid input -> INVALID_ARG; insufficient storage -> NO_SPACE. Storage must
 * be fresh or deinitialized; overwriting a live object is not supported.
 * Destination storage must not overlap configuration, source arrays or the
 * borrowed definition; such overlap returns INVALID_ARG before storage writes.
 * Overlapping output handles also return INVALID_ARG, leaving the aliased
 * output/input/storage unchanged instead of clearing the output handle. */
h2_pal_result_t h2_skeleton2d_definition_size(const h2_skeleton2d_config_t *,
                                              size_t *out_bytes);
h2_pal_result_t
h2_skeleton2d_definition_init(void *, size_t, const h2_skeleton2d_config_t *,
                              h2_skeleton2d_definition_t **out_definition);
h2_pal_result_t h2_skeleton2d_instance_size(const h2_skeleton2d_definition_t *,
                                            size_t *out_bytes);
h2_pal_result_t h2_skeleton2d_instance_init(void *, size_t,
                                            const h2_skeleton2d_definition_t *,
                                            h2_skeleton2d_t **out_instance);
/** Negative time is valid; repeat maps into [0,duration), clamp into
 * [0,duration]. Missing channels use defaults; sampling does not accumulate. */
h2_pal_result_t h2_skeleton2d_sample(h2_skeleton2d_t *, size_t, int64_t,
                                     h2_skeleton2d_loop_t,
                                     h2_skeleton2d_pose_slot_t);
/** Blend A/B into current, weight [0,1]; shortest angle difference in [-pi,pi).
 * All three slots begin at defaults. No implicit interpolation of part state.
 */
h2_pal_result_t h2_skeleton2d_blend(h2_skeleton2d_t *, double);
/** Sparse transactional overrides; duplicate IDs rejected. */
h2_pal_result_t h2_skeleton2d_set_local(h2_skeleton2d_t *,
                                        const h2_skeleton2d_local_t *, size_t);
h2_pal_result_t h2_skeleton2d_set_parts(h2_skeleton2d_t *,
                                        const h2_skeleton2d_part_state_t *,
                                        size_t);
/** Atomic matrix/order publication. Equal layers use ascending part ID. */
h2_pal_result_t h2_skeleton2d_evaluate(h2_skeleton2d_t *,
                                       const double root_matrix[6]);
/** INVALID_STATE before first evaluate/after deinit; clears output on error. */
h2_pal_result_t h2_skeleton2d_view(const h2_skeleton2d_t *,
                                   h2_skeleton2d_view_t *out_view);
/** Counts for allocating adapters; zero for invalid/deinitialized definition.
 */
size_t h2_skeleton2d_bone_count(const h2_skeleton2d_definition_t *);
size_t h2_skeleton2d_part_count(const h2_skeleton2d_definition_t *);
/** NULL and repeated deinit on still-live storage are allowed; never frees.
 * Deinitialize instances before their definition. */
void h2_skeleton2d_instance_deinit(h2_skeleton2d_t *);
void h2_skeleton2d_definition_deinit(h2_skeleton2d_definition_t *);
#ifdef __cplusplus
}
#endif
#endif
