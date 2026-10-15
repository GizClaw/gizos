#include "h2_f32_math.h"

#include <assert.h>
#include <fenv.h>
#include <stdio.h>

/* Frozen test-only oracle from the original 29-line renderer/native helper.
 * No production consumer links this second expression. */
static float original_div(float numerator, float denominator) {
  if (denominator < 0) { numerator = -numerator; denominator = -denominator; }
  if (!(denominator >= 1e-20f && denominator <= 1e20f) || fabsf(numerator) > 1e20f)
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

static uint32_t float_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static void check_original(float numerator, float denominator) {
  float actual = h2_f32_div(numerator, denominator);
  float expected = original_div(numerator, denominator);
  assert((isnan(actual) && isnan(expected)) || float_bits(actual) == float_bits(expected));
}

int main(void) {
  assert(fegetround() == FE_TONEAREST);
  const float edges[] = {0.0f, -0.0f, 1.0f, -1.0f, 1e-20f, -1e-20f,
      1e20f, -1e20f, FLT_MIN, -FLT_MIN, FLT_TRUE_MIN, -FLT_TRUE_MIN,
      FLT_MAX, -FLT_MAX, INFINITY, -INFINITY, NAN};
  for (size_t i = 0; i < sizeof(edges)/sizeof(edges[0]); ++i)
    for (size_t j = 0; j < sizeof(edges)/sizeof(edges[0]); ++j)
      check_original(edges[i], edges[j]);
  assert(!signbit(h2_f32_div(-0.0f, 1.0f)));
  assert(signbit(h2_f32_div(-0.0f, 1e-21f)));
  assert(isinf(h2_f32_div(1.0f, -0.0f)) && signbit(h2_f32_div(1.0f, -0.0f)));
  assert(isnan(h2_f32_div(0.0f, 0.0f)));
  uint32_t random = UINT32_C(0x18763542), max_ulp = 0;
  size_t normal_count = 0;
  for (size_t i = 0; i < 1000000; ++i) {
    random = random * UINT32_C(1664525) + UINT32_C(1013904223);
    uint32_t nb = random;
    random = random * UINT32_C(1664525) + UINT32_C(1013904223);
    uint32_t db = random;
    float numerator, denominator;
    memcpy(&numerator, &nb, sizeof(nb));
    memcpy(&denominator, &db, sizeof(db));
    check_original(numerator, denominator);
    float expected = numerator / denominator;
    float actual = h2_f32_div(numerator, denominator);
    if (isnormal(expected) && isnormal(actual) &&
        fabsf(numerator) <= 1e20f && fabsf(denominator) >= 1e-20f &&
        fabsf(denominator) <= 1e20f) {
      uint32_t a = float_bits(actual), b = float_bits(expected);
      uint32_t ulp = a > b ? a - b : b - a;
      if (ulp > max_ulp) max_ulp = ulp;
      ++normal_count;
    }
  }
  assert(normal_count > 100000);
  /* Regression ceiling for this deterministic sample, not an API guarantee. */
  assert(max_ulp <= 2);
  printf("F32_DIV original=PASS samples=1000000 normal=%zu max_ulp=%u\n",
         normal_count, (unsigned)max_ulp);
  return 0;
}
