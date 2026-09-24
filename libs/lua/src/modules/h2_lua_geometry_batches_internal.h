#ifndef H2_LUA_GEOMETRY_BATCHES_INTERNAL_H
#define H2_LUA_GEOMETRY_BATCHES_INTERNAL_H
#include "h2_lua_numeric_internal.h"
#define H2_GEOMETRY_BATCH_META "h2.geometry.batch"
#define H2_GEOMETRY_POSE_META "h2.geometry.pose"
typedef struct h2_geometry_part {
  size_t first, count;
  int kind;
} h2_geometry_part_t;
typedef struct h2_geometry_batch {
  size_t n, parts;
  double direction[4];
  double *base; /* packed x,y,w0,w1; immutable after construction */
  h2_geometry_part_t *topology;
} h2_geometry_batch_t;
typedef struct h2_geometry_pose {
  h2_geometry_batch_t *geometry; /* retained through userdata slot 1 */
  int valid, projected;
  double inputs[14];
  size_t generation;
  double *positions, *staged, *screen;
} h2_geometry_pose_t;
void h2_geometry_batches_register(lua_State *s);
#endif
