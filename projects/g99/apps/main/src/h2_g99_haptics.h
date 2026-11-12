#ifndef H2_G99_HAPTICS_H
#define H2_G99_HAPTICS_H

#include "h2_runtime.h"

typedef struct h2_g99_haptics {
  bool active;
  uint64_t stop_ms;
} h2_g99_haptics_t;

h2_pal_result_t h2_g99_haptics_pulse(h2_g99_haptics_t* haptics,
                                     h2_runtime_t* runtime, uint64_t now_ms);
h2_pal_result_t h2_g99_haptics_tick(h2_g99_haptics_t* haptics,
                                    h2_runtime_t* runtime, uint64_t now_ms);
h2_pal_result_t h2_g99_haptics_stop(h2_g99_haptics_t* haptics,
                                    h2_runtime_t* runtime);

#endif
