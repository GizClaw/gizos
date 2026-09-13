#ifndef H2_JIELI_WL82_ATOMIC_H
#define H2_JIELI_WL82_ATOMIC_H

/* Minimal 32-bit atomics for the wl82 PAL core.  The pi32v2 clang and the
 * Linux/macOS host compilers provide the GCC __atomic builtins; the Windows
 * host test build (MSVC) maps the same operations onto Interlocked*. */

#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h>

static inline uint32_t h2_jieli_atomic_load_u32(volatile uint32_t *address)
{
    return (uint32_t)_InterlockedCompareExchange((volatile long *)address, 0L, 0L);
}

static inline void h2_jieli_atomic_store_u32(volatile uint32_t *address, uint32_t value)
{
    (void)_InterlockedExchange((volatile long *)address, (long)value);
}

static inline uint32_t h2_jieli_atomic_fetch_sub_u32(volatile uint32_t *address, uint32_t value)
{
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)address, -(long)value);
}

static inline int h2_jieli_atomic_cas_u32(
    volatile uint32_t *address,
    uint32_t *expected,
    uint32_t desired)
{
    const uint32_t observed = (uint32_t)_InterlockedCompareExchange(
        (volatile long *)address, (long)desired, (long)*expected);
    if (observed == *expected) {
        return 1;
    }
    *expected = observed;
    return 0;
}
#else

static inline uint32_t h2_jieli_atomic_load_u32(volatile uint32_t *address)
{
    return __atomic_load_n(address, __ATOMIC_ACQUIRE);
}

static inline void h2_jieli_atomic_store_u32(volatile uint32_t *address, uint32_t value)
{
    __atomic_store_n(address, value, __ATOMIC_RELEASE);
}

static inline uint32_t h2_jieli_atomic_fetch_sub_u32(volatile uint32_t *address, uint32_t value)
{
    return __atomic_fetch_sub(address, value, __ATOMIC_ACQ_REL);
}

static inline int h2_jieli_atomic_cas_u32(
    volatile uint32_t *address,
    uint32_t *expected,
    uint32_t desired)
{
    return __atomic_compare_exchange_n(address, expected, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#endif

#endif /* H2_JIELI_WL82_ATOMIC_H */
