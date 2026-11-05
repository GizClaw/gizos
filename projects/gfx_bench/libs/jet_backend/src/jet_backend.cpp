#include "Scene.hpp"

#include "h2_car_model.h"
#include "h2_gfx_scene.h"

#include <cmath>
#include <cstdint>
#include <vector>
#include <new>
#include <utility>

/* Jet uses integer world coordinates; 256 units per scene unit keeps the car
 * near the 1024-unit size its fixed-point chain was tuned around. */
namespace {
using namespace Renderer;

constexpr float kWorldScale = 256.0f;
constexpr float kPi = 3.14159265f;

struct JetState {
  const h2_pal_mem_api_t* mem = nullptr;
  Scene* scene = nullptr;
  Camera camera;
  DirectionalLight* key = nullptr;
  AmbientLight* ambient = nullptr;
  Object* car = nullptr;
  Texture* livery = nullptr;
  Texture* reflection = nullptr;
  Material* paint = nullptr;
  Material* wheel = nullptr;
  Material* mirror = nullptr;
  uint16_t* sky = nullptr;
  int applied_look = -1;
} jet;

int32_t world(float value) { return (int32_t)std::lround(value * kWorldScale); }

int32_t fixed(float value) {
  return (int32_t)std::lround(value * (float)FIXED_POINT_SCALE);
}

Vector3 fixed_normal(const float normal[3]) {
  return {-fixed(normal[0]), fixed(normal[1]), fixed(normal[2])};
}

template <class T, class... Args>
T* allocate(Args&&... args) {
  void* storage = h2_pal_mem_alloc(jet.mem, sizeof(T));
  return storage ? new (storage) T(std::forward<Args>(args)...) : nullptr;
}

template <class T>
void release(T* object) {
  if (!object) return;
  object->~T();
  h2_pal_mem_free(jet.mem, object);
}

Material* make_material(Texture* texture, ShadingMode mode) {
  auto* material = allocate<Material>(0xFFFF);
  if (!material) return nullptr;
  material->diffuseMap = texture;
  material->shadingMode = mode;
  material->perspectiveCorrect = true;
  return material;
}

void add_triangles(const h2_car_range_t& range, Material* material) {
  const uint16_t* index = h2_car_model.indices + range.first * 3u;
  for (unsigned i = 0; i < range.count; ++i, index += 3)
    jet.car->addTriangle(index[0], index[2], index[1], material);
}

void set_material(const h2_car_range_t& range, unsigned offset,
                  Material* material) {
  for (unsigned i = 0; i < range.count; ++i)
    jet.car->triangles[offset + i].material = material;
}

void apply_look(h2_gfx_look_t look) {
  const bool hard = look == H2_GFX_LOOK_FACTORY;
  for (size_t i = 0; i < jet.car->vertices.size(); ++i) {
    const h2_car_vertex_t& source = h2_car_model.vertices[i];
    jet.car->vertices[i].normal =
        fixed_normal(hard ? source.hard_normal : source.smooth_normal);
    jet.car->vertices[i].uv = {fixed(source.uv[0]), fixed(source.uv[1])};
  }
  set_material(h2_car_model.body, h2_car_model.body.first,
               look == H2_GFX_LOOK_CHROME ? jet.mirror : jet.paint);
}

void update_reflections(const h2_car_range_t& range, float yaw) {
  const uint16_t* index = h2_car_model.indices + range.first * 3u;
  for (unsigned i = 0; i < range.count * 3u; ++i) {
    const unsigned vertex = index[i];
    float u = 0.0f, v = 0.0f;
    h2_gfx_reflection_uv(h2_car_model.vertices[vertex].smooth_normal, yaw, &u,
                         &v);
    jet.car->vertices[vertex].uv = {fixed(u), fixed(v)};
  }
}

int jet_init(const h2_gfx_target_t* target) {
  if (jet.mem || !target || !target->mem || !target->color || !target->depth ||
      !target->sky || target->width <= 0 || target->height <= 0)
    return -1;
  jet.mem = target->mem;
  jet.scene = allocate<Scene>(target->color, target->depth, target->width,
                              target->height);
  if (!jet.scene) return -1;
  jet.sky = const_cast<uint16_t*>(target->sky);
  jet.scene->backgroundGradientColors = jet.sky;
  jet.scene->setClearBuffer(true);

  jet.camera.setFOV(H2_GFX_FOV_DEGREES, target->width);
  jet.camera.nearPlane = world(H2_GFX_NEAR);
  jet.camera.farPlane = world(H2_GFX_FAR);
  jet.camera.setPosition(world(h2_gfx_camera_eye.x), world(h2_gfx_camera_eye.y),
                         world(h2_gfx_camera_eye.z));
  jet.camera.lookAt(Vector3{world(h2_gfx_camera_target.x),
                            world(h2_gfx_camera_target.y),
                            world(h2_gfx_camera_target.z)});
  jet.scene->setCamera(&jet.camera);

  const h2_gfx_vec3_t& light = h2_gfx_light_direction;
  const int32_t elevation =
      (int32_t)std::lround(std::asin(light.y) * 180.0f / kPi);
  const int32_t azimuth =
      (int32_t)std::lround(std::atan2(light.z, -light.x) * 180.0f / kPi);
  jet.key = allocate<DirectionalLight>(Vector3{azimuth, elevation, 0},
                                       Color{255, 250, 244}, 255);
  jet.ambient = allocate<AmbientLight>(Color{96, 96, 108});
  if (!jet.key || !jet.ambient) return -1;
  jet.scene->setDirectionalLight(jet.key);
  jet.scene->setAmbientLight(jet.ambient);

  jet.livery = allocate<Texture>(H2_CAR_LIVERY_SIZE, H2_CAR_LIVERY_SIZE,
                                 const_cast<uint16_t*>(h2_car_model.livery));
  jet.reflection =
      allocate<Texture>(H2_CAR_REFLECTION_SIZE, H2_CAR_REFLECTION_SIZE,
                        const_cast<uint16_t*>(h2_car_model.reflection));
  if (!jet.livery || !jet.reflection) return -1;
  jet.livery->bilinear = false;
  jet.reflection->bilinear = false;

  jet.paint = make_material(jet.livery, ShadingMode::PHONG);
  if (!jet.paint) return -1;
  jet.paint->diffuse = 170;
  jet.paint->specular = 205;
  jet.paint->specularExponent = 32;
  jet.wheel = make_material(jet.livery, ShadingMode::UNLIT);
  if (!jet.wheel) return -1;
  jet.wheel->specular = 0;
  jet.mirror = make_material(jet.reflection, ShadingMode::UNLIT);
  if (!jet.mirror) return -1;
  jet.mirror->specular = 0;

  jet.car = allocate<Object>();
  if (!jet.car) return -1;
  for (unsigned i = 0; i < h2_car_model.vertex_count; ++i) {
    const h2_car_vertex_t& source = h2_car_model.vertices[i];
    Object::Vertex vertex;
    vertex.position = {-world(source.position[0]), world(source.position[1]),
                       world(source.position[2])};
    vertex.normal = fixed_normal(source.hard_normal);
    vertex.uv = {fixed(source.uv[0]), fixed(source.uv[1])};
    jet.car->addVertex(vertex);
  }
  add_triangles(h2_car_model.body, jet.paint);
  add_triangles(h2_car_model.wheels, jet.wheel);
  add_triangles(h2_car_model.windows, jet.mirror);
  jet.car->calculateBoundingBox();
  jet.car->cachePositions();
  jet.scene->addObject(jet.car);
  return 0;
}

void jet_render(const h2_gfx_view_t* view) {
  if (jet.applied_look != (int)view->look) {
    apply_look(view->look);
    jet.applied_look = (int)view->look;
  }
  update_reflections(h2_car_model.windows, view->yaw);
  if (view->look == H2_GFX_LOOK_CHROME)
    update_reflections(h2_car_model.body, view->yaw);
  // Jet uses the opposite screen-X handedness to tgx/OpenGL. Reflect the
  // world across X, reverse winding, and negate yaw to keep touch and poses
  // identical when changing renderer.
  jet.car->setPosition(-world(view->x), 0, world(view->z));
  jet.car->setRotation(0, -(int32_t)std::lround(view->yaw * 180.0f / kPi), 0);
  jet.scene->render();
}
void jet_deinit() {
  release(jet.scene);
  release(jet.car);
  release(jet.paint);
  release(jet.wheel);
  release(jet.mirror);
  release(jet.livery);
  release(jet.reflection);
  release(jet.key);
  release(jet.ambient);
  jet = JetState{};
}
}  // namespace

extern "C" const h2_gfx_backend_t h2_gfx_backend_jet = {
    "JET",
    jet_init,
    jet_render,
    jet_deinit,
};
