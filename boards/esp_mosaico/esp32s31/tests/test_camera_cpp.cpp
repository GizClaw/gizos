#include "h2_mosaico_camera.h"
#include <cassert>

// Link this C++ consumer against the actual C adapter in camera_test.
extern "C" void h2_test_camera_cpp_linkage(void) {
    assert(h2_mosaico_camera() != nullptr);
}
