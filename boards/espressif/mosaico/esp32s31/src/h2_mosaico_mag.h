#ifndef H2_MOSAICO_MAG_H
#define H2_MOSAICO_MAG_H
#include <stdint.h>
/* Convert compensated microtesla to milligauss, preserving Bosch overflow
 * output INT16_MIN as -327680, not a reliable signed field measurement. */
static inline int32_t h2_mosaico_mag_axis(int32_t value) { return value * 10; }
#endif
