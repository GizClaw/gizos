#ifndef H2_JIELI_AC791N_CLOCK_MATH_H
#define H2_JIELI_AC791N_CLOCK_MATH_H

#include <stdint.h>

/* TIMER5 uses the oscillator divided by 8192. Keep the oscillator remainder
 * until conversion so non-integral tick durations never accumulate drift. */
#define H2_JIELI_CLOCK_DIVIDER 8192u
#define H2_JIELI_CLOCK_PERIOD 65535u

static inline uint64_t h2_jieli_clock_ticks_to_us(uint64_t ticks, uint32_t oscillator_hz)
{
    const uint64_t scale = (uint64_t)H2_JIELI_CLOCK_DIVIDER * 1000000u;
    return (ticks / oscillator_hz) * scale +
           ((ticks % oscillator_hz) * scale) / oscillator_hz;
}

static inline uint64_t h2_jieli_clock_snapshot_ticks(
    uint64_t completed_ticks, uint32_t counter, int overflow_pending)
{
    return completed_ticks + counter +
           (overflow_pending ? H2_JIELI_CLOCK_PERIOD : 0u);
}

#endif
