#include "h2_desktop_arena_diagnostics_probe.h"

#include <execinfo.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

static h2_pal_result_t safe_copy(void *user, const void *source,
                                 void *destination, size_t bytes) {
    (void)user;
    mach_vm_size_t copied = 0u;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)source,
                               bytes, (mach_vm_address_t)destination,
                               &copied) != KERN_SUCCESS || copied != bytes)
        return H2_PAL_ERR_UNSUPPORTED;
    return H2_PAL_OK;
}

static h2_pal_result_t capture_frames(void *user, uintptr_t *frames,
                                       size_t capacity, size_t *out_count) {
    (void)user;
    if (frames == NULL || out_count == NULL || capacity == 0u)
        return H2_PAL_ERR_INVALID_ARG;
    void *raw[16] = {0};
    const int count = backtrace(raw, 16);
    size_t copied = 0u;
    for (int i = 2; i < count && copied < capacity; ++i)
        frames[copied++] = (uintptr_t)raw[i];
    *out_count = copied;
    return copied == 0u ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_OK;
}

h2_mem_arena_diagnostics_probe_t h2_desktop_arena_diagnostics_probe(void) {
    return (h2_mem_arena_diagnostics_probe_t){
        .safe_copy = safe_copy, .capture_frames = capture_frames};
}
