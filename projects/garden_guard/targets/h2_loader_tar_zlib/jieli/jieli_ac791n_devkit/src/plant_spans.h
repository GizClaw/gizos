#ifndef H2_GARDEN_GUARD_JIELI_PLANT_SPANS_H
#define H2_GARDEN_GUARD_JIELI_PLANT_SPANS_H

#include "h2_garden_guard.h"
#include "xz.h"

typedef struct h2_garden_guard_jieli_plant_spans {
  h2_runtime_t* runtime;
  uint8_t* output;
  struct xz_dec* decoder;
  struct xz_buf buffer;
  unsigned prepared;
  bool finished;
} h2_garden_guard_jieli_plant_spans_t;

void h2_garden_guard_jieli_prepare_plant_spans(void* user);
const uint8_t* h2_garden_guard_jieli_find_plant_spans(
    void* user, const h2_garden_guard_plant_span_key_t* key);
void h2_garden_guard_jieli_release_plant_spans(
    h2_garden_guard_jieli_plant_spans_t* spans);

#endif
