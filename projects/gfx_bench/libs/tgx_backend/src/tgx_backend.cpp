#include "tgx.h"

#include "h2_car_model.h"
#include "h2_gfx_scene.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
using namespace tgx;

constexpr Shader kLoadedShaders =
    (Shader)(SHADER_PERSPECTIVE | SHADER_ZBUFFER | SHADER_GOURAUD |
             SHADER_UNLIT | SHADER_TEXTURE | SHADER_TEXTURE_NEAREST |
             SHADER_TEXTURE_WRAP_POW2);
constexpr float kPi = 3.14159265f;

using Renderer = Renderer3D<RGB565, kLoadedShaders, uint16_t>;

struct TgxState {
  Image<RGB565>* frame = nullptr;
  Image<RGB565>* livery = nullptr;
  Image<RGB565>* reflection = nullptr;
  Renderer* renderer = nullptr;
  fVec3* positions = nullptr;
  fVec3* hard_normals = nullptr;
  fVec3* smooth_normals = nullptr;
  fVec2* base_uvs = nullptr;
  fVec2* reflection_uvs = nullptr;
  uint16_t* color = nullptr;
  const uint16_t* sky = nullptr;
  const h2_pal_mem_api_t* mem = nullptr;
  int width = 0;
  int height = 0;
} state;

const uint16_t* indices_of(const h2_car_range_t& range) {
  return h2_car_model.indices + range.first * 3u;
}

void clear_frame() {
  uint16_t* out = state.color;
  for (int y = 0; y < state.height; ++y) {
    const uint16_t color = state.sky[y];
    for (int x = 0; x < state.width; ++x) *out++ = color;
  }
}

void update_reflections(const h2_car_range_t& range, float yaw) {
  const uint16_t* index = indices_of(range);
  for (unsigned i = 0; i < range.count * 3u; ++i) {
    const unsigned vertex = index[i];
    float u = 0.0f, v = 0.0f;
    h2_gfx_reflection_uv(h2_car_model.vertices[vertex].smooth_normal, yaw, &u,
                         &v);
    state.reflection_uvs[vertex] = fVec2(u, v);
  }
}

void draw_lit(const h2_car_range_t& range, const fVec3* normals) {
  const uint16_t* index = indices_of(range);
  state.renderer->setShaders((Shader)(SHADER_GOURAUD | SHADER_TEXTURE));
  state.renderer->drawTriangles(range.count, index, state.positions, index,
                                normals, index, state.base_uvs, state.livery);
}

void draw_unlit(const h2_car_range_t& range, const fVec2* uvs,
                const Image<RGB565>* texture) {
  const uint16_t* index = indices_of(range);
  state.renderer->setShaders((Shader)(SHADER_UNLIT | SHADER_TEXTURE));
  state.renderer->drawTriangles(range.count, index, state.positions, nullptr,
                                nullptr, index, uvs, texture);
}

int tgx_init(const h2_gfx_target_t* target) {
  if (state.mem || !target || !target->mem || !target->color ||
      !target->depth || !target->sky || target->width <= 0 ||
      target->height <= 0)
    return -1;
  state.mem = target->mem;
  const size_t count = h2_car_model.vertex_count;
  state.color = target->color;
  state.sky = target->sky;
  state.width = target->width;
  state.height = target->height;
  state.positions = (fVec3*)h2_pal_mem_alloc(state.mem, sizeof(fVec3) * count);
  state.hard_normals =
      (fVec3*)h2_pal_mem_alloc(state.mem, sizeof(fVec3) * count);
  state.smooth_normals =
      (fVec3*)h2_pal_mem_alloc(state.mem, sizeof(fVec3) * count);
  state.base_uvs = (fVec2*)h2_pal_mem_alloc(state.mem, sizeof(fVec2) * count);
  state.reflection_uvs =
      (fVec2*)h2_pal_mem_alloc(state.mem, sizeof(fVec2) * count);
  if (!state.positions || !state.hard_normals || !state.smooth_normals ||
      !state.base_uvs || !state.reflection_uvs)
    return -1;
  for (size_t i = 0; i < count; ++i) {
    const h2_car_vertex_t& source = h2_car_model.vertices[i];
    new (&state.positions[i])
        fVec3(source.position[0], source.position[1], source.position[2]);
    new (&state.hard_normals[i]) fVec3(
        source.hard_normal[0], source.hard_normal[1], source.hard_normal[2]);
    new (&state.smooth_normals[i])
        fVec3(source.smooth_normal[0], source.smooth_normal[1],
              source.smooth_normal[2]);
    new (&state.base_uvs[i]) fVec2(source.uv[0], source.uv[1]);
    new (&state.reflection_uvs[i]) fVec2(state.base_uvs[i]);
  }

  void* frame_storage = h2_pal_mem_alloc(state.mem, sizeof(Image<RGB565>));
  void* livery_storage = h2_pal_mem_alloc(state.mem, sizeof(Image<RGB565>));
  void* reflection_storage = h2_pal_mem_alloc(state.mem, sizeof(Image<RGB565>));
  void* renderer_storage = h2_pal_mem_alloc(state.mem, sizeof(Renderer));
  if (!frame_storage || !livery_storage || !reflection_storage ||
      !renderer_storage) {
    h2_pal_mem_free(state.mem, frame_storage);
    h2_pal_mem_free(state.mem, livery_storage);
    h2_pal_mem_free(state.mem, reflection_storage);
    h2_pal_mem_free(state.mem, renderer_storage);
    return -1;
  }
  state.frame = new (frame_storage)
      Image<RGB565>(target->color, target->width, target->height);
  state.livery = new (livery_storage)
      Image<RGB565>(const_cast<uint16_t*>(h2_car_model.livery),
                    H2_CAR_LIVERY_SIZE, H2_CAR_LIVERY_SIZE);
  state.reflection = new (reflection_storage)
      Image<RGB565>(const_cast<uint16_t*>(h2_car_model.reflection),
                    H2_CAR_REFLECTION_SIZE, H2_CAR_REFLECTION_SIZE);

  Renderer* renderer = new (renderer_storage) Renderer();
  state.renderer = renderer;
  renderer->setViewportSize(target->width, target->height);
  renderer->setOffset(0, 0);
  renderer->setImage(state.frame);
  renderer->setZbuffer(target->depth);
  renderer->setPerspective(H2_GFX_FOV_DEGREES,
                           (float)target->width / (float)target->height,
                           H2_GFX_NEAR, H2_GFX_FAR);
  renderer->setLookAt(
      fVec3(h2_gfx_camera_eye.x, h2_gfx_camera_eye.y, h2_gfx_camera_eye.z),
      fVec3(h2_gfx_camera_target.x, h2_gfx_camera_target.y,
            h2_gfx_camera_target.z),
      fVec3(0.0f, 1.0f, 0.0f));
  renderer->setCulling(1);
  renderer->setTextureQuality(SHADER_TEXTURE_NEAREST);
  renderer->setTextureWrappingMode(SHADER_TEXTURE_WRAP_POW2);
  renderer->setLight(fVec3(-h2_gfx_light_direction.x, -h2_gfx_light_direction.y,
                           -h2_gfx_light_direction.z),
                     RGBf(0.45f, 0.45f, 0.5f), RGBf(0.95f, 0.93f, 0.9f),
                     RGBf(1.0f, 1.0f, 1.0f));
  renderer->setMaterial(RGBf(1.0f, 1.0f, 1.0f), 0.85f, 0.7f, 0.8f, 32);
  return 0;
}

void tgx_render(const h2_gfx_view_t* view) {
  Renderer* renderer = state.renderer;
  clear_frame();
  renderer->clearZbuffer();
  renderer->setModelPosScaleRot(
      fVec3(view->x, 0.0f, view->z), fVec3(1.0f, 1.0f, 1.0f),
      view->yaw * 180.0f / kPi, fVec3(0.0f, 1.0f, 0.0f));
  update_reflections(h2_car_model.windows, view->yaw);
  if (view->look == H2_GFX_LOOK_CHROME) {
    update_reflections(h2_car_model.body, view->yaw);
    draw_unlit(h2_car_model.body, state.reflection_uvs, state.reflection);
  } else {
    draw_lit(h2_car_model.body, view->look == H2_GFX_LOOK_FACTORY
                                    ? state.hard_normals
                                    : state.smooth_normals);
  }
  draw_unlit(h2_car_model.wheels, state.base_uvs, state.livery);
  draw_unlit(h2_car_model.windows, state.reflection_uvs, state.reflection);
}
void tgx_deinit() {
  if (state.renderer) state.renderer->~Renderer();
  if (state.frame) state.frame->~Image();
  if (state.livery) state.livery->~Image();
  if (state.reflection) state.reflection->~Image();
  h2_pal_mem_free(state.mem, state.renderer);
  h2_pal_mem_free(state.mem, state.frame);
  h2_pal_mem_free(state.mem, state.livery);
  h2_pal_mem_free(state.mem, state.reflection);
  h2_pal_mem_free(state.mem, state.positions);
  h2_pal_mem_free(state.mem, state.hard_normals);
  h2_pal_mem_free(state.mem, state.smooth_normals);
  h2_pal_mem_free(state.mem, state.base_uvs);
  h2_pal_mem_free(state.mem, state.reflection_uvs);
  state = TgxState{};
}
}  // namespace

extern "C" const h2_gfx_backend_t h2_gfx_backend_tgx = {
    "TGX",
    tgx_init,
    tgx_render,
    tgx_deinit,
};
