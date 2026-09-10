#include "h2_lvgl_platform.h"

#include "lvgl.h"

#include <stddef.h>
#include <string.h>

typedef struct h2_lvgl_web_platform_state {
  const h2_pal_mem_api_t *allocator;
  const h2_pal_task_api_t *task_api;
  const h2_pal_sync_api_t *sync_api;
  const h2_pal_queue_api_t *queue_api;
  const h2_pal_time_api_t *time_api;
  int initialized;
} h2_lvgl_web_platform_state_t;

static h2_lvgl_web_platform_state_t h2_lvgl_web_platform;

static int h2_lvgl_web_platform_ready(void) {
  return h2_lvgl_web_platform.initialized &&
         h2_lvgl_web_platform.allocator != NULL &&
         h2_lvgl_web_platform.task_api != NULL &&
         h2_lvgl_web_platform.sync_api != NULL &&
         h2_lvgl_web_platform.queue_api != NULL &&
         h2_lvgl_web_platform.time_api != NULL;
}

int h2_lvgl_platform_init(const h2_lvgl_platform_config_t *config) {
  if (config == NULL || config->allocator == NULL || config->task_api == NULL ||
      config->sync_api == NULL || config->queue_api == NULL ||
      config->time_api == NULL || h2_lvgl_web_platform.initialized) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_lvgl_web_platform = (h2_lvgl_web_platform_state_t){
      .allocator = config->allocator,
      .task_api = config->task_api,
      .sync_api = config->sync_api,
      .queue_api = config->queue_api,
      .time_api = config->time_api,
      .initialized = 1,
  };
  return H2_PAL_OK;
}

void h2_lvgl_platform_deinit(void) {
  memset(&h2_lvgl_web_platform, 0, sizeof(h2_lvgl_web_platform));
}

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *memory, size_t bytes) {
  (void)memory;
  (void)bytes;
  return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t size) {
  return h2_lvgl_web_platform_ready()
             ? h2_pal_mem_alloc(h2_lvgl_web_platform.allocator, size)
             : NULL;
}

void *lv_realloc_core(void *memory, size_t new_size) {
  if (!h2_lvgl_web_platform_ready()) {
    return NULL;
  }
  if (memory == NULL) {
    return h2_pal_mem_alloc(h2_lvgl_web_platform.allocator, new_size);
  }
  return h2_pal_mem_realloc(h2_lvgl_web_platform.allocator, memory, new_size);
}

void lv_free_core(void *memory) {
  if (h2_lvgl_web_platform_ready() && memory != NULL) {
    h2_pal_mem_free(h2_lvgl_web_platform.allocator, memory);
  }
}

void lv_mem_monitor_core(lv_mem_monitor_t *monitor) {
  if (monitor != NULL) {
    memset(monitor, 0, sizeof(*monitor));
  }
}

lv_result_t lv_mem_test_core(void) {
  void *memory = lv_malloc_core(1u);
  if (memory == NULL) {
    return LV_RESULT_INVALID;
  }
  lv_free_core(memory);
  return LV_RESULT_OK;
}
