#ifndef H2_DESKTOP_RESOURCE_STATS_INTERNAL_H
#define H2_DESKTOP_RESOURCE_STATS_INTERNAL_H

#include <cstddef>

enum class h2_desktop_resource_kind {
  task,
  queue,
  mutex,
  semaphore,
  condition,
  timer,
  firmware_info,
};

void h2_desktop_resource_acquire(h2_desktop_resource_kind kind);
void h2_desktop_resource_release(h2_desktop_resource_kind kind);
void h2_desktop_task_stack_acquire(size_t bytes);
void h2_desktop_task_stack_release(size_t bytes);

#endif
