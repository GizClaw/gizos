#include "h2_atomic_unsupported_impl.h"

#include <assert.h>

int main(void) {
    h2_atomic_int_t integer = {0};
    h2_atomic_uint_t unsigned_integer = {0};
    h2_atomic_u8_t byte = {0};
    h2_atomic_u16_t halfword = {0};
    h2_atomic_u32_t word = {0};
    h2_atomic_size_t count = {0};
    h2_atomic_bool_t boolean = {0};
    h2_atomic_ptr_t pointer = {0};
    h2_atomic_flag_t flag = {0};

    assert(h2_atomic_int_init(&integer, 1) == H2_ATOMIC_UNSUPPORTED);
    assert(h2_atomic_uint_init(&unsigned_integer, 1u) == H2_ATOMIC_UNSUPPORTED);
    assert(h2_atomic_u8_init(&byte, 1u) == H2_ATOMIC_UNSUPPORTED);
    assert(h2_atomic_u16_init(&halfword, 1u) == H2_ATOMIC_UNSUPPORTED);
    assert(h2_atomic_u32_init(&word, 1u) == H2_ATOMIC_UNSUPPORTED);
    assert(h2_atomic_size_init(&count, 1u) == H2_ATOMIC_UNSUPPORTED);
    assert(h2_atomic_bool_init(&boolean, true) == H2_ATOMIC_UNSUPPORTED);
    assert(h2_atomic_ptr_init(&pointer, &word) == H2_ATOMIC_UNSUPPORTED);
    assert(h2_atomic_flag_init(&flag) == H2_ATOMIC_UNSUPPORTED);
    assert(integer.storage == NULL && unsigned_integer.storage == NULL);
    assert(byte.storage == NULL && halfword.storage == NULL);
    assert(word.storage == NULL && count.storage == NULL);
    assert(boolean.storage == NULL && pointer.storage == NULL);
    h2_atomic_int_destroy(&integer);
    h2_atomic_uint_destroy(&unsigned_integer);
    h2_atomic_u8_destroy(&byte);
    h2_atomic_u16_destroy(&halfword);
    h2_atomic_u32_destroy(&word);
    h2_atomic_size_destroy(&count);
    h2_atomic_bool_destroy(&boolean);
    h2_atomic_ptr_destroy(&pointer);
    h2_atomic_flag_destroy(&flag);
    return 0;
}
