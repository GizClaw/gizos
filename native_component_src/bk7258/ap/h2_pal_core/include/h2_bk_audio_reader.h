#ifndef H2_BK_AUDIO_READER_H
#define H2_BK_AUDIO_READER_H

#include <stdatomic.h>
#include <stdbool.h>

/** C11 AP BSP helpers for one reader and a serialized control owner. Both flags
 * must live in SRAM. Publish the borrow before checking admission so closing
 * admission followed by an idle observation excludes any later SDK read.
 * Hold the borrow through processing/publication. The owner pauses/wakes the
 * SDK reader, waits for idle, and only then closes/destroys its resources. */
/** @brief Acquire the reader borrow if admission is enabled.
 * @param enabled Required distinct SRAM flag owned by the controller. Closing
 * admission must use a sequentially consistent store.
 * @param borrowed Required SRAM flag published only by the single reader.
 * @return true with the borrow held, or false with no borrow. Task context
 * only.
 */
static inline bool h2_bk_audio_reader_enter(atomic_bool* enabled,
                                            atomic_bool* borrowed) {
  atomic_store_explicit(borrowed, true, memory_order_seq_cst);
  if (atomic_load_explicit(enabled, memory_order_seq_cst)) return true;
  atomic_store_explicit(borrowed, false, memory_order_seq_cst);
  return false;
}
/** @brief Release the required SRAM borrow flag after all resource use ends.
 * Call once for each successful enter, from the same serialized reader.
 */
static inline void h2_bk_audio_reader_leave(atomic_bool* borrowed) {
  atomic_store_explicit(borrowed, false, memory_order_seq_cst);
}
/** @brief Observe whether the required SRAM flag has no active borrower.
 * Only a controller that has closed admission may use true to reclaim storage.
 */
static inline bool h2_bk_audio_reader_idle(const atomic_bool* borrowed) {
  return !atomic_load_explicit(borrowed, memory_order_seq_cst);
}

#endif
