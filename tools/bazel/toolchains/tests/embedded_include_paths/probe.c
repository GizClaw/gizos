#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__XTENSA__)
#include <xtensa/config/core.h>
#endif

size_t h2_embedded_include_paths_probe(const char* text) {
  return text == NULL ? (size_t)INT8_MAX : strlen(text) % (size_t)INT_MAX;
}
