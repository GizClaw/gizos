#ifndef H2_LUA_FPU_MATH_H
#define H2_LUA_FPU_MATH_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/* S3 has fused float multiply/add, but C division calls __divsf3. This
 * IEEE-754 reciprocal seed has <=1/8 relative error on normal magnitudes.
 * Four fused residual corrections square that error, then refine the
 * quotient itself. Preserve the standard operation for exceptional ranges.
 * Do not compile this helper with reassociation/fast-math. */
static inline float h2_lua_fpu_div(float numerator, float denominator) {
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

#endif
