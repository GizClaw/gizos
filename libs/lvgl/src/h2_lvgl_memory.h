#ifndef H2_LVGL_MEMORY_H
#define H2_LVGL_MEMORY_H

#include "h2_lvgl_platform.h"

int h2_lvgl_memory_init(const h2_lvgl_platform_config_t *config, int threaded);
int h2_lvgl_memory_prepare(void);
void h2_lvgl_memory_release(void);
void h2_lvgl_memory_deinit(void);
void *h2_lvgl_memory_alloc(size_t size);
void *h2_lvgl_memory_realloc(void *ptr, size_t size);
void h2_lvgl_memory_free(void *ptr);

#endif
