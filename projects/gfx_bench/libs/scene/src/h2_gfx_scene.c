#include "h2_gfx_scene.h"

#include "h2_car_model.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265f
#define TAP_SLOP_PIXELS 14.0f
#define TURN_RATE 9.0f
#define IDLE_RADIANS_PER_SECOND 0.35f
#define MIN_HEADING_STEP 0.02f
#define LIMIT_X 1.7f
#define LIMIT_Z 1.5f

const h2_gfx_vec3_t h2_gfx_camera_eye = {0.0f, 4.2f, 8.0f};
const h2_gfx_vec3_t h2_gfx_camera_target = {0.0f, 0.45f, 0.0f};
const h2_gfx_vec3_t h2_gfx_light_direction = {-0.42f, 0.78f, 0.46f};

static h2_gfx_vec3_t normalized(h2_gfx_vec3_t v) {
  const float length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
  return (h2_gfx_vec3_t){v.x / length, v.y / length, v.z / length};
}

static h2_gfx_vec3_t cross(h2_gfx_vec3_t a, h2_gfx_vec3_t b) {
  return (h2_gfx_vec3_t){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                         a.x * b.y - a.y * b.x};
}

static float dot(h2_gfx_vec3_t a, h2_gfx_vec3_t b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static void camera_basis(h2_gfx_vec3_t* right, h2_gfx_vec3_t* up,
                         h2_gfx_vec3_t* forward) {
  const h2_gfx_vec3_t world_up = {0.0f, 1.0f, 0.0f};
  *forward =
      normalized((h2_gfx_vec3_t){h2_gfx_camera_target.x - h2_gfx_camera_eye.x,
                                 h2_gfx_camera_target.y - h2_gfx_camera_eye.y,
                                 h2_gfx_camera_target.z - h2_gfx_camera_eye.z});
  *right = normalized(cross(*forward, world_up));
  *up = cross(*right, *forward);
}

void h2_gfx_reflection_uv(const float normal[3], float yaw, float* u,
                          float* v) {
  h2_gfx_vec3_t right, up, forward;
  camera_basis(&right, &up, &forward);
  const float cosine = cosf(yaw), sine = sinf(yaw);
  const h2_gfx_vec3_t world = {cosine * normal[0] + sine * normal[2], normal[1],
                               cosine * normal[2] - sine * normal[0]};
  *u = 0.5f + 0.49f * dot(world, right);
  *v = 0.5f - 0.49f * dot(world, up);
}

int h2_gfx_screen_to_ground(float screen_x, float screen_y, int width,
                            int height, float* x, float* z) {
  h2_gfx_vec3_t right, up, forward;
  camera_basis(&right, &up, &forward);
  const float half = tanf(H2_GFX_FOV_DEGREES * 0.5f * PI_F / 180.0f);
  const float nx = (screen_x + 0.5f) / (float)width * 2.0f - 1.0f;
  const float ny = 1.0f - (screen_y + 0.5f) / (float)height * 2.0f;
  const h2_gfx_vec3_t ray = {
      forward.x + right.x * nx * half + up.x * ny * half,
      forward.y + right.y * nx * half + up.y * ny * half,
      forward.z + right.z * nx * half + up.z * ny * half,
  };
  if (ray.y >= -1e-4f) return 0;
  const float t = -h2_gfx_camera_eye.y / ray.y;
  *x = h2_gfx_camera_eye.x + ray.x * t;
  *z = h2_gfx_camera_eye.z + ray.z * t;
  return 1;
}

static int hits_car(const h2_gfx_interaction_t* state, float x, float y) {
  h2_gfx_vec3_t right, up, forward;
  camera_basis(&right, &up, &forward);
  const h2_gfx_view_t* view = &state->view;
  const float cosine = cosf(view->yaw), sine = sinf(view->yaw);
  const float focal =
      (float)state->height / (2.0f * tanf(H2_GFX_FOV_DEGREES * PI_F / 360.0f));
  float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
  /* Project the eight AABB corners, including the roof. A ground-only hit
   * test makes a tap on the windscreen accidentally switch renderers. */
  for (int i = 0; i < 8; ++i) {
    const float lx = (i & 1 ? 1.0f : -1.0f) * h2_car_model.half_width;
    const float lz = (i & 2 ? 1.0f : -1.0f) * h2_car_model.half_length;
    const h2_gfx_vec3_t relative = {
        cosine * lx + sine * lz + view->x - h2_gfx_camera_eye.x,
        (i & 4 ? h2_car_model.height : 0.0f) - h2_gfx_camera_eye.y,
        cosine * lz - sine * lx + view->z - h2_gfx_camera_eye.z,
    };
    const float depth = dot(relative, forward);
    if (depth <= H2_GFX_NEAR) continue;
    const float sx =
        (float)state->width * 0.5f + dot(relative, right) * focal / depth;
    const float sy =
        (float)state->height * 0.5f - dot(relative, up) * focal / depth;
    min_x = fminf(min_x, sx);
    max_x = fmaxf(max_x, sx);
    min_y = fminf(min_y, sy);
    max_y = fmaxf(max_y, sy);
  }
  return x >= min_x - 8.0f && x <= max_x + 8.0f && y >= min_y - 8.0f &&
         y <= max_y + 8.0f;
}

static float clamp(float value, float limit) {
  return value < -limit ? -limit : value > limit ? limit : value;
}

static float wrapped(float radians) {
  while (radians > PI_F) radians -= 2.0f * PI_F;
  while (radians < -PI_F) radians += 2.0f * PI_F;
  return radians;
}

void h2_gfx_interaction_init(h2_gfx_interaction_t* state, int width,
                             int height) {
  memset(state, 0, sizeof(*state));
  state->width = width;
  state->height = height;
  state->view.yaw = 0.7f;
  state->target_yaw = state->view.yaw;
}

h2_gfx_action_t h2_gfx_interaction_touch(h2_gfx_interaction_t* state,
                                         h2_gfx_touch_t kind, float x,
                                         float y) {
  float ground_x = 0.0f, ground_z = 0.0f;
  const int grounded = h2_gfx_screen_to_ground(
      x, y, state->width, state->height, &ground_x, &ground_z);
  switch (kind) {
    case H2_GFX_TOUCH_DOWN:
      state->pressed = 1;
      state->moved = 0;
      state->start_x = x;
      state->start_y = y;
      state->on_car = grounded && hits_car(state, x, y);
      state->grab_x = state->view.x - ground_x;
      state->grab_z = state->view.z - ground_z;
      state->last_x = state->view.x;
      state->last_z = state->view.z;
      return H2_GFX_ACTION_NONE;
    case H2_GFX_TOUCH_MOVE:
      if (!state->pressed) return H2_GFX_ACTION_NONE;
      if (fabsf(x - state->start_x) > TAP_SLOP_PIXELS ||
          fabsf(y - state->start_y) > TAP_SLOP_PIXELS)
        state->moved = 1;
      if (state->on_car && state->moved && grounded) {
        state->view.x = clamp(ground_x + state->grab_x, LIMIT_X);
        state->view.z = clamp(ground_z + state->grab_z, LIMIT_Z);
        const float dx = state->view.x - state->last_x;
        const float dz = state->view.z - state->last_z;
        if (dx * dx + dz * dz > MIN_HEADING_STEP * MIN_HEADING_STEP) {
          state->target_yaw = atan2f(dx, dz);
          state->last_x = state->view.x;
          state->last_z = state->view.z;
        }
      }
      return H2_GFX_ACTION_NONE;
    case H2_GFX_TOUCH_UP:
    default: {
      const int tap = state->pressed && !state->moved &&
                      fabsf(x - state->start_x) <= TAP_SLOP_PIXELS &&
                      fabsf(y - state->start_y) <= TAP_SLOP_PIXELS;
      const int on_car = state->on_car;
      state->pressed = 0;
      state->on_car = 0;
      if (!tap) return H2_GFX_ACTION_NONE;
      return on_car ? H2_GFX_ACTION_NEXT_LOOK : H2_GFX_ACTION_NEXT_BACKEND;
    }
  }
}

void h2_gfx_interaction_step(h2_gfx_interaction_t* state, float seconds) {
  if (state->on_car && state->moved) {
    const float blend = 1.0f - expf(-TURN_RATE * seconds);
    state->view.yaw = wrapped(
        state->view.yaw + wrapped(state->target_yaw - state->view.yaw) * blend);
  } else if (!state->pressed) {
    state->view.yaw =
        wrapped(state->view.yaw + IDLE_RADIANS_PER_SECOND * seconds);
    state->target_yaw = state->view.yaw;
  }
}
