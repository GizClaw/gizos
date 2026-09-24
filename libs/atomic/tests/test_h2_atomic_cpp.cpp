#include "h2_atomic.h"

#include <cassert>
#include <type_traits>

static_assert(std::is_standard_layout<h2_atomic_flag_t>::value);
static_assert(sizeof(h2_atomic_flag_t) == sizeof(void *));

int main() {
    h2_atomic_flag_t flag = {};
    assert(h2_atomic_flag_init(&flag) == H2_ATOMIC_OK);
    assert(!h2_atomic_flag_test_and_set(&flag, H2_ATOMIC_ACQUIRE));
    h2_atomic_flag_clear(&flag, H2_ATOMIC_RELEASE);
    h2_atomic_flag_destroy(&flag);
    assert(flag.storage == nullptr);
    return 0;
}
