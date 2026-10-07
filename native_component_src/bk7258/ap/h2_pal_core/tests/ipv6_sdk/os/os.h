#include <stddef.h>
#include <stdint.h>
void test_after_atomic_store(void);
static inline void test_atomic_store(uint32_t *pointer, uint32_t value,
                                     int ordering) {
  __atomic_store_n(pointer, value, ordering);
  test_after_atomic_store();
}
/* Observe actual production publication between each atomic store. */
#define __atomic_store_n(pointer, value, ordering) \
  test_atomic_store(pointer, value, ordering)
