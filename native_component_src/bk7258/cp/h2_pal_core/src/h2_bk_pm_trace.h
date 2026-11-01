#ifndef H2_BK_PM_TRACE_H
#define H2_BK_PM_TRACE_H

#include <stdbool.h>
#include <stdint.h>

/* Temporary R14 diagnostic format. A different image family must use a
 * different magic so an earlier trial cannot become a new reset's evidence. */
#define H2_BK_PM_TRACE_MAGIC UINT32_C(0x52313454)

enum {
  H2_BK_PM_TRACE_SAMPLE = 0,
  H2_BK_PM_TRACE_TRANSITION = 1,
  H2_BK_PM_TRACE_HANDSHAKE_ENTER = 2,
  H2_BK_PM_TRACE_HANDSHAKE_EXIT = 3,
  H2_BK_PM_TRACE_NORMAL_ENTER = 4,
  H2_BK_PM_TRACE_NORMAL_EXIT = 5,
  H2_BK_PM_TRACE_FAULT_ENTRY = 6,
  H2_BK_PM_TRACE_FAULT_PC = 7,
  H2_BK_PM_TRACE_BOOT = 8,
  H2_BK_PM_TRACE_AON_STOP_ENTER = 9,
  H2_BK_PM_TRACE_AON_STOP_EXIT = 10,
  H2_BK_PM_TRACE_TICKLESS_ENTER = 11,
  H2_BK_PM_TRACE_TICKLESS_EXIT = 12,
  H2_BK_PM_TRACE_RTC_ENTER = 13,
  H2_BK_PM_TRACE_RTC_EXIT = 14,
  H2_BK_PM_TRACE_STEP_ENTER = 15,
  H2_BK_PM_TRACE_STEP_EXIT = 16,
  H2_BK_PM_TRACE_IRQ_LOOKUP = 17,
  H2_BK_PM_TRACE_IRQ_CALLBACK = 18,
  H2_BK_PM_TRACE_SLEEP_RETURN_FLASH_OWNER = 20,
  H2_BK_PM_TRACE_CRITICAL_ENTER = 21,
  H2_BK_PM_TRACE_CRITICAL_ENTERED = 22,
  H2_BK_PM_TRACE_CRITICAL_EXIT = 23,
  H2_BK_PM_TRACE_CRITICAL_EXITED_CONTEXT = 24,
  H2_BK_PM_TRACE_YIELD_ENTER = 25,
  H2_BK_PM_TRACE_YIELD_EXIT = 26,
  H2_BK_PM_TRACE_FLUSH_ENTER = 27,
  H2_BK_PM_TRACE_FLUSH_EXIT = 28,
  H2_BK_PM_TRACE_CRITICAL_ENTER_CONTEXT = 29,
  H2_BK_PM_TRACE_TICKLESS_RETURN_CONTEXT = 30,
  H2_BK_PM_TRACE_VECTOR_BASE = 32,
  H2_BK_PM_TRACE_VECTOR_COUNT = 80,
  H2_BK_PM_TRACE_MAX_EVENT =
      H2_BK_PM_TRACE_VECTOR_BASE + H2_BK_PM_TRACE_VECTOR_COUNT - 1,
};

static inline __attribute__((always_inline)) bool h2_bk_pm_trace_event_valid(
    uint32_t event) {
  return event <= H2_BK_PM_TRACE_IRQ_CALLBACK ||
         (event >= H2_BK_PM_TRACE_SLEEP_RETURN_FLASH_OWNER &&
          event <= H2_BK_PM_TRACE_TICKLESS_RETURN_CONTEXT) ||
         (event >= H2_BK_PM_TRACE_VECTOR_BASE &&
          event <= H2_BK_PM_TRACE_MAX_EVENT);
}

typedef struct {
  uint32_t magic;
  uint32_t sequence;
  uint32_t meta;
  uint32_t value;
  uint32_t check;
} h2_bk_pm_trace_record_t;

typedef h2_bk_pm_trace_record_t h2_bk_pm_trace_slots_t[2];

_Static_assert(sizeof(h2_bk_pm_trace_record_t) == 20u,
               "retained trace record layout changed");
_Static_assert(sizeof(h2_bk_pm_trace_slots_t) == 40u,
               "retained trace slots exceed the audited gap");

#ifndef H2_BK_PM_TRACE_STORE
#define H2_BK_PM_TRACE_STORE(address, value) (*(address) = (value))
#endif

#ifndef H2_BK_PM_TRACE_BARRIER
#if defined(__arm__)
#define H2_BK_PM_TRACE_BARRIER() __asm volatile("dmb sy" ::: "memory")
#else
#define H2_BK_PM_TRACE_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif
#endif

static inline __attribute__((always_inline)) uint32_t
h2_bk_pm_trace_check(uint32_t sequence, uint32_t meta, uint32_t value) {
  uint32_t hash = UINT32_C(2166136261) ^ H2_BK_PM_TRACE_MAGIC;
  hash = (hash ^ sequence) * UINT32_C(16777619);
  hash = (hash ^ meta) * UINT32_C(16777619);
  return (hash ^ value) * UINT32_C(16777619);
}

static inline __attribute__((always_inline)) void h2_bk_pm_trace_publish(
    volatile h2_bk_pm_trace_record_t* target, uint32_t sequence, uint32_t meta,
    uint32_t value) {
  H2_BK_PM_TRACE_STORE(&target->magic, 0u);
  H2_BK_PM_TRACE_BARRIER();
  H2_BK_PM_TRACE_STORE(&target->sequence, sequence);
  H2_BK_PM_TRACE_STORE(&target->meta, meta);
  H2_BK_PM_TRACE_STORE(&target->value, value);
  H2_BK_PM_TRACE_STORE(&target->check,
                       h2_bk_pm_trace_check(sequence, meta, value));
  H2_BK_PM_TRACE_BARRIER();
  /* Publish validity last; the other slot remains the previous complete event.
   */
  H2_BK_PM_TRACE_STORE(&target->magic, H2_BK_PM_TRACE_MAGIC);
  H2_BK_PM_TRACE_BARRIER();
}

static inline bool h2_bk_pm_trace_read(
    const volatile h2_bk_pm_trace_record_t* source,
    h2_bk_pm_trace_record_t* output) {
  output->magic = 0u;
  if (source->magic != H2_BK_PM_TRACE_MAGIC) return false;
  H2_BK_PM_TRACE_BARRIER();
  const uint32_t sequence = source->sequence;
  const uint32_t meta = source->meta;
  const uint32_t value = source->value;
  const uint32_t check = source->check;
  H2_BK_PM_TRACE_BARRIER();
  if (source->magic != H2_BK_PM_TRACE_MAGIC ||
      check != h2_bk_pm_trace_check(sequence, meta, value) ||
      !h2_bk_pm_trace_event_valid(meta >> 16) || (meta & 0xffffu) > 9u)
    return false;
  output->magic = H2_BK_PM_TRACE_MAGIC;
  output->sequence = sequence;
  output->meta = meta;
  output->value = value;
  output->check = check;
  return true;
}

static inline bool h2_bk_pm_trace_newer(uint32_t left, uint32_t right) {
  const uint32_t distance = left - right;
  return distance != 0u && distance < UINT32_C(0x80000000);
}

#endif
