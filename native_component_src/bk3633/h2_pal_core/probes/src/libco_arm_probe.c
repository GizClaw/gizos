#if !defined(__arm__) || !defined(__ARM_ARCH) || __ARM_ARCH != 5
#error "BK3633 libco requires the 32-bit ARMv5 backend"
#endif

#if defined(__thumb__)
#error "BK3633 libco context switch must compile in ARM mode"
#endif

#include "h2_libco.h"
#include "libco.h"

#include <stdint.h>

_Static_assert(sizeof(void *) == 4u, "BK3633 pointers must be 32-bit");
_Static_assert(H2_LIBCO_STACK_ALIGNMENT == 16u,
               "libco initial stack alignment changed");

int main(void) {
    cothread_t (*active_fn)(void) = co_active;
    cothread_t (*derive_fn)(void *, unsigned int, void (*)(void)) = co_derive;
    void (*switch_fn)(cothread_t) = co_switch;
    return active_fn == 0 || derive_fn == 0 || switch_fn == 0;
}
