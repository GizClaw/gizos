#include "h2_atomic.h"
#include "test_h2_atomic_static_bridge.h"

#include <cassert>
#include <type_traits>

static_assert(std::is_standard_layout<h2_atomic_flag_t>::value);
static_assert(sizeof(h2_atomic_flag_t) == sizeof(void *));

int main() {
    auto *static_int = h2_test_static_int();
    auto *static_bool = h2_test_static_bool();
    auto *static_ptr = h2_test_static_ptr();
    auto *static_flag = h2_test_static_flag();
    assert(static_int->storage != nullptr && static_bool->storage != nullptr);
    assert(static_ptr->storage != nullptr && static_flag->storage != nullptr);
    assert(static_cast<const void *>(static_int->storage) !=
           static_cast<const void *>(static_bool->storage));
    assert(static_cast<const void *>(static_ptr->storage) !=
           static_cast<const void *>(static_flag->storage));
    assert(h2_atomic_int_load(static_int, H2_ATOMIC_RELAXED) == 7);
    assert(!h2_atomic_bool_exchange(static_bool, true, H2_ATOMIC_ACQ_REL));
    assert(h2_atomic_ptr_exchange(static_ptr, static_int, H2_ATOMIC_ACQ_REL) == nullptr);
    assert(!h2_atomic_flag_test_and_set(static_flag, H2_ATOMIC_ACQUIRE));
    assert(h2_atomic_flag_test_and_set(static_flag, H2_ATOMIC_ACQUIRE));
    h2_atomic_flag_clear(static_flag, H2_ATOMIC_RELEASE);
    assert(h2_atomic_flag_init(static_flag) == H2_ATOMIC_INVALID_STATE);
    h2_atomic_int_destroy(static_int);
    h2_atomic_bool_destroy(static_bool);
    h2_atomic_ptr_destroy(static_ptr);
    h2_atomic_flag_destroy(static_flag);
    assert(static_int->storage != nullptr && static_bool->storage != nullptr);
    assert(static_ptr->storage != nullptr && static_flag->storage != nullptr);

    h2_atomic_flag_t flag = {};
    assert(h2_atomic_flag_init(&flag) == H2_ATOMIC_OK);
    assert(!h2_atomic_flag_test_and_set(&flag, H2_ATOMIC_ACQUIRE));
    h2_atomic_flag_clear(&flag, H2_ATOMIC_RELEASE);
    h2_atomic_flag_destroy(&flag);
    assert(flag.storage == nullptr);
    return 0;
}
