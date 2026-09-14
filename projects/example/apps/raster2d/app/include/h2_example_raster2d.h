#ifndef H2_EXAMPLE_RASTER2D_H
#define H2_EXAMPLE_RASTER2D_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Draw a fixed public pattern on a 240x240 Display; owns its acquisition. */
h2_pal_result_t h2_example_raster2d_run(h2_runtime_t *runtime);
#ifdef __cplusplus
}
#endif
#endif
