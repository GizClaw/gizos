#ifndef H2_ATOMIC_TEST_SOC_H
#define H2_ATOMIC_TEST_SOC_H
#include <stdint.h>
extern uintptr_t h2_test_extram_low;
extern uintptr_t h2_test_extram_high;
#define SOC_EXTRAM_DATA_LOW h2_test_extram_low
#define SOC_EXTRAM_DATA_HIGH h2_test_extram_high
#endif
