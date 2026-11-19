#ifndef H2_DISPLAY_BENCH_H
#define H2_DISPLAY_BENCH_H

#include "hal/h2_pal_display.h"
#include "os/h2_pal_mem.h"
#include "os/h2_pal_time.h"

#include <stdbool.h>

typedef enum h2_display_bench_mode {
  H2_DISPLAY_BENCH_SUBMIT,
  H2_DISPLAY_BENCH_COPY,
  H2_DISPLAY_BENCH_SYNC_TRANSFER,
  H2_DISPLAY_BENCH_CLOSE_FENCE,
} h2_display_bench_mode_t;

/* One exclusive Display owner runs the bench once per boot. Pixel storage is
 * borrowed only through draw_bitmap. The completed-pixel counter is sampled
 * in the same task before and after each synchronous update. */
typedef struct h2_display_bench_profile {
  const char* board;
  const char* version;
  const char* bus;
  const char* panel_config;
  h2_display_bench_mode_t mode;
  const h2_pal_display_api_t* display;
  const h2_pal_mem_api_t* memory;
  const h2_pal_time_api_t* time;
  void (*line)(const char* text);
  uint32_t (*completed_pixels)(void);
  bool (*timed_out)(void);
  unsigned clock_tick_ns;
} h2_display_bench_profile_t;

int h2_display_bench_run(const h2_display_bench_profile_t* profile);

/* After the measured suite, submit one full checkerboard and leave Display
 * open. The caller remains the exclusive Display owner. */
int h2_display_bench_show_preview(const h2_display_bench_profile_t* profile);

#endif
