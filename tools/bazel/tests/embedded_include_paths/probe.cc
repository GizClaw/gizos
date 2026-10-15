#include <limits.h>
#include <stddef.h>
#include <stdint.h>

// C++ library headers exist only where the toolchain ships them.
#if defined(__has_include)
#if __has_include(<cstddef>)
#include <cstddef>
#endif
#endif

extern "C" size_t h2_embedded_include_paths_cxx_probe(size_t value) {
    return value % static_cast<size_t>(INT_MAX) + static_cast<size_t>(INT8_MAX);
}
