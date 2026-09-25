#include "h2_desktop_arena_diagnostics_probe.h"

#undef NDEBUG
#include <assert.h>
#include <string.h>

int main(void) {
    const h2_mem_arena_diagnostics_probe_t probe =
        h2_desktop_arena_diagnostics_probe();
    assert(probe.safe_copy != NULL && probe.capture_frames != NULL);
    const char source[] = "concurrent-safe-copy";
    char copied[sizeof(source)] = {0};
    assert(probe.safe_copy(probe.user, source, copied, sizeof(source)) ==
           H2_PAL_OK);
    assert(memcmp(source, copied, sizeof(source)) == 0);
    uintptr_t frames[8] = {0};
    size_t count = 0u;
    assert(probe.capture_frames(probe.user, frames, 8u, &count) == H2_PAL_OK);
    assert(count != 0u && frames[0] != 0u);
}
