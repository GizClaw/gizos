#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BK_OK 0
#if CONFIG_H2_BK_PLATFORM_CP_COREDUMP
#define H2_CD_CORE() 0u
#else
#define H2_CD_CORE() 1u
#endif
typedef struct {
  union {
    struct {
      uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12;
      uint32_t sp, lr, pc, xpsr, msp, psp, primask, basepri, faultmask, control,
          fpscr;
    };
    uint32_t cm33_regs[24];
  };
} SAVED_CONTEXT;
typedef struct {
  uint32_t CFSR, HFSR, BFAR, MMFAR;
} test_scb_t;
extern test_scb_t test_scb;
#define SCB (&test_scb)
bool bk_flash_is_driver_inited(void);
uint32_t bk_flash_get_current_total_size(void);
int bk_flash_read_bytes(uint32_t address, uint8_t* data, uint32_t size);
int bk_flash_dump_write(uint32_t address, const uint8_t* data, uint32_t size);
void test_copy_stack(void* data, uint32_t address, uint32_t size);
#define H2_CD_COPY(data, address, size) test_copy_stack(data, address, size)

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
/* Native BK uses GCC atomics. The host-only SDK fixture provides equivalent
 * full-barrier MSVC operations, without changing the ARM implementation. */
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_RELAXED 0
static inline uint32_t h2_cd_test_atomic_load(volatile uint32_t* value) {
  return (uint32_t)_InterlockedCompareExchange((volatile long*)value, 0, 0);
}
static inline bool h2_cd_test_atomic_compare(volatile uint32_t* value,
                                             uint32_t* expected,
                                             uint32_t desired) {
  const uint32_t previous = (uint32_t)_InterlockedCompareExchange(
      (volatile long*)value, (long)desired, (long)*expected);
  if (previous == *expected) return true;
  *expected = previous;
  return false;
}
#define __atomic_load_n(value, order) h2_cd_test_atomic_load(value)
#define __atomic_compare_exchange_n(value, expected, desired, weak, success, \
                                    failure)                                 \
  h2_cd_test_atomic_compare(value, expected, desired)
#endif
