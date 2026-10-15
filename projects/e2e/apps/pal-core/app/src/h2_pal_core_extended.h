#ifndef H2_PAL_CORE_EXTENDED_H
#define H2_PAL_CORE_EXTENDED_H
#include "h2_pal_core_e2e.h"
typedef struct h2_pal_core_extended h2_pal_core_extended_t;
h2_pal_result_t h2_pal_core_extended_run(h2_runtime_t *runtime,
                                         const h2_pal_core_e2e_config_t *config,
                                         size_t index,
                                         h2_pal_core_extended_t **owner);
h2_pal_result_t h2_pal_core_extended_cleanup(h2_pal_core_extended_t **owner);
#endif
