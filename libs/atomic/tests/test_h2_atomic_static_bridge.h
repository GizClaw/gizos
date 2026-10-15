#ifndef TEST_H2_ATOMIC_STATIC_BRIDGE_H
#define TEST_H2_ATOMIC_STATIC_BRIDGE_H

#include "h2_atomic.h"

H2_ATOMIC_DECLARE_STATIC(int, h2_test_static_int);
H2_ATOMIC_DECLARE_STATIC(bool, h2_test_static_bool);
H2_ATOMIC_DECLARE_STATIC(ptr, h2_test_static_ptr);
H2_ATOMIC_DECLARE_STATIC(flag, h2_test_static_flag);

#endif
