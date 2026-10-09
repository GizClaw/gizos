#ifndef H2_MOSAICO_DISPLAY_H
#define H2_MOSAICO_DISPLAY_H
#include "h2_pal.h"
int h2_mosaico_display_init(void);
const h2_pal_display_api_t *h2_mosaico_display(void);
const h2_pal_touch_api_t *h2_mosaico_touch(void);
int h2_mosaico_display_deinit(void);
#endif
