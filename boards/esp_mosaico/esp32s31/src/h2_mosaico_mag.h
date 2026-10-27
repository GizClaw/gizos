#ifndef H2_MOSAICO_MAG_H
#define H2_MOSAICO_MAG_H
#include <stdint.h>
#include <stdbool.h>
/* Bosch compensated saturation sentinel (microtesla), not a measurement. */
static inline bool h2_mosaico_mag_valid(int32_t value) {
  return value != INT16_MIN;
}
static inline int32_t h2_mosaico_mag_axis(int32_t value) { return value * 10; }
#endif
