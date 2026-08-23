#include "global_mouse_source.h"

namespace h2::starboy {

void *find_global_mouse_window(const char *) { return nullptr; }

bool sample_global_mouse_relative(void *, std::int32_t *, std::int32_t *) {
  return false;
}

}  // namespace h2::starboy
