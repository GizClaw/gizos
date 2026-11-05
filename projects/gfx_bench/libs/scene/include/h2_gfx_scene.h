#ifndef H2_GFX_SCENE_H
#define H2_GFX_SCENE_H

#include "h2/pal/os/h2_pal_mem.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* World units: Y up, the car stands on Y = 0 and is four units long. */
#define H2_GFX_FOV_DEGREES 38.0f
#define H2_GFX_NEAR 1.0f
#define H2_GFX_FAR 24.0f

typedef struct h2_gfx_vec3 {
  float x;
  float y;
  float z;
} h2_gfx_vec3_t;

extern const h2_gfx_vec3_t h2_gfx_camera_eye;
extern const h2_gfx_vec3_t h2_gfx_camera_target;
/* Unit vector from the scene toward the key light. */
extern const h2_gfx_vec3_t h2_gfx_light_direction;

typedef enum h2_gfx_look {
  H2_GFX_LOOK_FACTORY = 0, /* Livery, hard normals, reflective windows. */
  H2_GFX_LOOK_POLISHED,    /* Livery, smoothed normals. */
  H2_GFX_LOOK_CHROME,      /* Reflection probe across the body. */
  H2_GFX_LOOK_COUNT,
} h2_gfx_look_t;

typedef struct h2_gfx_view {
  float x;
  float z;
  float yaw; /* Radians around +Y. */
  h2_gfx_look_t look;
} h2_gfx_view_t;

/* The caller owns both buffers; depth has one uint16_t per pixel. */
typedef struct h2_gfx_target {
  uint16_t* color;
  uint16_t* depth;
  int width;
  int height;
  const uint16_t* sky; /* One RGB565 colour per row. */
  const h2_pal_mem_api_t*
      mem; /* Borrowed until deinit; owns adapter storage. */
} h2_gfx_target_t;

/* Local evaluation singleton per backend: init once, render on the caller's
 * task, deinit before releasing target buffers. Duplicate init is rejected.
 * init returns 0 or an allocation/configuration error. deinit also releases
 * partial initialization. Jet's upstream containers retain its default C++
 * allocator; the adapter itself and tgx use the injected Memory PAL. */
typedef struct h2_gfx_backend {
  const char* name;
  int (*init)(const h2_gfx_target_t* target);
  void (*render)(const h2_gfx_view_t* view);
  void (*deinit)(void);
} h2_gfx_backend_t;

extern const h2_gfx_backend_t h2_gfx_backend_tgx;
extern const h2_gfx_backend_t h2_gfx_backend_jet;

/* Probe coordinates for a model-space normal rotated by yaw. */
void h2_gfx_reflection_uv(const float normal[3], float yaw, float* u, float* v);

/* Map a screen point onto the ground. Returns 0 when the ray misses it. */
int h2_gfx_screen_to_ground(float screen_x, float screen_y, int width,
                            int height, float* x, float* z);

typedef enum h2_gfx_touch {
  H2_GFX_TOUCH_DOWN = 0,
  H2_GFX_TOUCH_MOVE,
  H2_GFX_TOUCH_UP,
} h2_gfx_touch_t;

typedef enum h2_gfx_action {
  H2_GFX_ACTION_NONE = 0,
  H2_GFX_ACTION_NEXT_LOOK,
  H2_GFX_ACTION_NEXT_BACKEND,
} h2_gfx_action_t;

/* Single-writer gesture state, including the car's pose. */
typedef struct h2_gfx_interaction {
  h2_gfx_view_t view;
  float target_yaw;
  float grab_x;
  float grab_z;
  float last_x;
  float last_z;
  float start_x;
  float start_y;
  int width;
  int height;
  int pressed;
  int on_car;
  int moved;
} h2_gfx_interaction_t;

void h2_gfx_interaction_init(h2_gfx_interaction_t* state, int width,
                             int height);
h2_gfx_action_t h2_gfx_interaction_touch(h2_gfx_interaction_t* state,
                                         h2_gfx_touch_t kind, float x, float y);
/* Advance turning and idle rotation by seconds. */
void h2_gfx_interaction_step(h2_gfx_interaction_t* state, float seconds);

#ifdef __cplusplus
}
#endif

#endif
