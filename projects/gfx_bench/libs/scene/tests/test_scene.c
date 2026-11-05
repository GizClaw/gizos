#include "h2_gfx_scene.h"

#include <assert.h>
#include <math.h>

/* Locate the projection of the ground origin independently of gesture code. */
static void ground_origin(float* x, float* y) {
  float best = 1e9f;
  for (int sy = 0; sy < 480; sy += 4)
    for (int sx = 0; sx < 480; sx += 4) {
      float gx = 0.0f, gz = 0.0f;
      if (!h2_gfx_screen_to_ground((float)sx, (float)sy, 480, 480, &gx, &gz))
        continue;
      const float distance = gx * gx + gz * gz;
      if (distance < best) {
        best = distance;
        *x = (float)sx;
        *y = (float)sy;
      }
    }
}

int main(void) {
  float cx = 0.0f, cy = 0.0f;
  ground_origin(&cx, &cy);
  h2_gfx_interaction_t state;
  h2_gfx_interaction_init(&state, 480, 480);
  h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_DOWN, cx, cy);
  assert(h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_UP, cx, cy) ==
         H2_GFX_ACTION_NEXT_LOOK);
  assert(state.view.x == 0.0f && state.view.z == 0.0f);

  h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_DOWN, 20.0f, 20.0f);
  assert(h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_UP, 20.0f, 20.0f) ==
         H2_GFX_ACTION_NEXT_BACKEND);

  h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_DOWN, cx, cy);
  for (int i = 1; i <= 10; ++i) {
    h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_MOVE, cx + (float)(i * 8),
                             cy);
    h2_gfx_interaction_step(&state, 0.03f);
  }
  assert(state.view.x > 0.5f);
  assert(fabsf(state.view.z) < 0.2f);
  assert(fabsf(state.view.yaw - 1.5708f) < 0.25f);
  assert(h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_UP, cx + 80.0f, cy) ==
         H2_GFX_ACTION_NONE);

  h2_gfx_interaction_init(&state, 480, 480);
  h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_DOWN, cx, cy);
  h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_MOVE, 479.0f, cy);
  assert(state.view.x <= 1.7f);

  h2_gfx_interaction_init(&state, 480, 480);
  h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_DOWN, 20.0f, 20.0f);
  h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_MOVE, 200.0f, 200.0f);
  assert(state.view.x == 0.0f && state.view.z == 0.0f);

  h2_gfx_interaction_init(&state, 480, 480);
  /* A missed MOVE edge must not turn a drag into a material/backend tap. */
  h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_DOWN, cx, cy);
  assert(h2_gfx_interaction_touch(&state, H2_GFX_TOUCH_UP, cx + 60.0f, cy) ==
         H2_GFX_ACTION_NONE);
  const float yaw = state.view.yaw;
  h2_gfx_interaction_step(&state, 1.0f);
  assert(state.view.yaw > yaw);
  return 0;
}
