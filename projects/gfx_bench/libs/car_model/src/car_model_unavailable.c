#include "h2_car_model.h"

/* A clean checkout has no licensed car asset. Keep the build graph usable and
 * the generic gesture tests independent of that optional local input. The App
 * rejects this descriptor with a clear diagnostic before opening the display.
 * These are generic hit-test dimensions, not model vertex data. */
const h2_car_model_t h2_car_model = {
    .half_width = 0.8f,
    .height = 1.1f,
    .half_length = 2.0f,
};
