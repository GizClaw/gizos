#ifndef H2_LUA_NUMERIC_COMPENSATED_INTERNAL_H
#define H2_LUA_NUMERIC_COMPENSATED_INTERNAL_H
#include <math.h>
/* Two-float scratch preserves residuals; published world state is binary64. */
typedef struct {
  float hi, lo;
} precise_float;
#if defined(_MSC_VER)
#define H2_COMPENSATED_INLINE __forceinline
#else
#define H2_COMPENSATED_INLINE inline __attribute__((always_inline))
#endif
static H2_COMPENSATED_INLINE precise_float pf_from(double x) {
  float hi = (float)x;
  return (precise_float){hi, (float)(x - (double)hi)};
}
static H2_COMPENSATED_INLINE precise_float pf_add(precise_float a,
                                                  precise_float b) {
  float sum = a.hi + b.hi, v = sum - a.hi;
  float error = (a.hi - (sum - v)) + (b.hi - v) + a.lo + b.lo;
  float hi = sum + error;
  return (precise_float){hi, error - (hi - sum)};
}
static H2_COMPENSATED_INLINE precise_float pf_sub(precise_float a,
                                                  precise_float b) {
  return pf_add(a, (precise_float){-b.hi, -b.lo});
}
static H2_COMPENSATED_INLINE precise_float pf_square(precise_float a) {
  float product = a.hi * a.hi,
        error = fmaf(a.hi, a.hi, -product) + 2.0f * a.hi * a.lo + a.lo * a.lo;
  float hi = product + error;
  return (precise_float){hi, error - (hi - product)};
}
/* Float square-root seed followed by a double Newton refinement. World
 * state remains double; for moderate magnitudes the seed's relative
 * error is squared (~1e-14). Extreme values retain the libm reference path. */
static double refined_sqrt(double value) {
  if (value < 1e-20 || value > 1e20)
    return sqrt(value);
  double root = (double)sqrtf((float)value);
  return .5 * (root + value / root);
}
#endif
