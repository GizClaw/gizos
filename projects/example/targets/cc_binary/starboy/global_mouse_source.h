#ifndef H2_STARBOY_GLOBAL_MOUSE_SOURCE_H
#define H2_STARBOY_GLOBAL_MOUSE_SOURCE_H

#include <cstdint>

namespace h2::starboy {

void *find_global_mouse_window(const char *window_title);
bool sample_global_mouse_relative(void *window,
                                  std::int32_t *out_x,
                                  std::int32_t *out_y);

}  // namespace h2::starboy

#endif
