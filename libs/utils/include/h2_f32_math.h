#ifndef H2_F32_MATH_H
#define H2_F32_MATH_H

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#if FLT_RADIX != 2 || FLT_MANT_DIG != 24 || FLT_MAX_EXP != 128 || FLT_EVAL_METHOD != 0
#error "h2_f32_div requires binary32 evaluation without excess precision"
#endif
#if defined(__FAST_MATH__) || defined(__ASSOCIATIVE_MATH__) || \
    (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0) || defined(_M_FP_FAST)
#error "h2_f32_div does not support fast-math or reassociation"
#endif

#ifdef __cplusplus
static_assert(sizeof(float) == sizeof(uint32_t), "binary32 storage required");
#else
_Static_assert(sizeof(float) == sizeof(uint32_t), "binary32 storage required");
#endif

/**
 * @brief Divide binary32 values using a reciprocal seed and fused corrections.
 *
 * Intended for portable raster/native numeric consumers on targets where float
 * division is expensive. Requires IEEE binary32 storage/evaluation, round to
 * nearest, and no fast-math, reassociation or flush-to-zero mode. Explicit fmaf
 * calls must retain fused semantics. Has no state, allocation or blocking.
 *
 * A negative denominator is normalized first. Denominators outside the inclusive
 * [1e-20f,1e20f] range, numerator magnitudes above 1e20f, and nonfinite initial
 * quotients fall back to C division of those normalized operands. Otherwise four
 * fused reciprocal corrections and one quotient residual correction are used.
 * This preserves the original renderer/native helper algorithm; it is not a
 * correctly-rounded replacement for every IEEE division. In particular the
 * refined path can return positive zero for a negative-zero numerator. NaN
 * payload/sign and floating-point exception flags are not part of this contract.
 * Numerical sample tests do not establish a whole-domain ULP bound.
 *
 * @param numerator Dividend, including exceptional IEEE values.
 * @param denominator Divisor, including exceptional IEEE values.
 * @return Refined quotient or the specified native-division fallback.
 */
static inline float h2_f32_div(float numerator, float denominator) {
  if (denominator < 0) {
    numerator = -numerator;
    denominator = -denominator;
  }
  if (!(denominator >= 1e-20f && denominator <= 1e20f) ||
      fabsf(numerator) > 1e20f)
    return numerator / denominator;
  uint32_t bits;
  memcpy(&bits, &denominator, sizeof(bits));
  bits = UINT32_C(0x7f000000) - bits;
  float inverse;
  memcpy(&inverse, &bits, sizeof(inverse));
  for (int i = 0; i < 4; ++i)
    inverse = fmaf(inverse, fmaf(-denominator, inverse, 1.0f), inverse);
  float quotient = numerator * inverse;
  if (!isfinite(quotient)) return numerator / denominator;
  return fmaf(fmaf(-quotient, denominator, numerator), inverse, quotient);
}

#endif
