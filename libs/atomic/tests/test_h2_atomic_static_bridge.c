#include "test_h2_atomic_static_bridge.h"

#include "h2_atomic_static.h"

H2_ATOMIC_DEFINE_STATIC_ACCESSOR(int, h2_test_static_int, 7);
H2_ATOMIC_DEFINE_STATIC_ACCESSOR(bool, h2_test_static_bool, false);
H2_ATOMIC_DEFINE_STATIC_ACCESSOR(ptr, h2_test_static_ptr, NULL);
H2_ATOMIC_DEFINE_STATIC_ACCESSOR(flag, h2_test_static_flag, 0u);
