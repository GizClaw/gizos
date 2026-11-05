#ifndef H2_GFX_BENCH_H
#define H2_GFX_BENCH_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gfx_bench_config {
  /* Optional, called once on this task after the first successful panel
   * submission. A nonzero result ends the App. user is borrowed for the run. */
  int (*ready)(void* user);
  void* user;
} h2_gfx_bench_config_t;

/* Local visual evaluation App. Call once on a dedicated task with >=64 KiB
 * stack, an initialized Runtime, Display/Touch/Memory/Time/Log capabilities.
 * No Runtime input polling may be active: this App owns one touch-sampling
 * worker, forwarding ordered pointer edges through a bounded PAL queue.
 * Runs until a PAL failure; recovery/exit is handled by the launcher's Loader.
 * Opens display and touch, owns both backends and their buffers, and releases
 * them on failure. The launcher must keep Runtime/providers alive after return
 * (a provider close failure can retain a provider-owned buffer). */
int h2_gfx_bench_run(h2_runtime_t* runtime,
                     const h2_gfx_bench_config_t* config);

#ifdef __cplusplus
}
#endif
#endif
