#ifndef H2_CAR_MODEL_H
#define H2_CAR_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Static car mesh shared by every renderer. Ranges count triangles. */
typedef struct h2_car_vertex {
  float position[3];
  float hard_normal[3];
  float smooth_normal[3];
  float uv[2];
} h2_car_vertex_t;

typedef struct h2_car_range {
  uint16_t first;
  uint16_t count;
} h2_car_range_t;

enum {
  H2_CAR_LIVERY_SIZE = 256,
  H2_CAR_REFLECTION_SIZE = 128,
};

typedef struct h2_car_model {
  const h2_car_vertex_t* vertices;
  uint16_t vertex_count;
  const uint16_t* indices;
  uint16_t triangle_count;
  h2_car_range_t body;
  h2_car_range_t wheels;
  h2_car_range_t windows;
  const uint16_t* livery;     /* RGB565, H2_CAR_LIVERY_SIZE square. */
  const uint16_t* reflection; /* RGB565, H2_CAR_REFLECTION_SIZE square. */
  float half_width;
  float height;
  float half_length;
} h2_car_model_t;

extern const h2_car_model_t h2_car_model;

#ifdef __cplusplus
}
#endif

#endif
