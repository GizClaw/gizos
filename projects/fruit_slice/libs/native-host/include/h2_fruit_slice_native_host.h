#ifndef H2_FRUIT_SLICE_NATIVE_HOST_H
#define H2_FRUIT_SLICE_NATIVE_HOST_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif

/** Native entry defaults: every capability explicitly unsupported. The target
 * supplies board/target/chip, Core, Display, Touch and optional sandbox
 * storage. */
h2_runtime_config_t h2_fruit_slice_native_defaults(void);

/** Read a thread-safe platform pointer snapshot in logical display pixels.
 * The callback and user remain borrowed for the Touch adapter's lifetime. */
typedef h2_pal_result_t (*h2_fruit_slice_pointer_read_fn)(void* user,
                                                          int32_t* x,
                                                          int32_t* y,
                                                          int* pressed);

typedef struct h2_fruit_slice_pointer {
  h2_pal_touch_api_t api;
  void* user;
  h2_fruit_slice_pointer_read_fn read;
  int32_t width, height, last_x, last_y;
  bool opened, pressed;
} h2_fruit_slice_pointer_t;

/** Initialize caller-owned storage. One App thread owns open/poll/close;
 * native UI threads only update the platform snapshot through its own API.
 * Incomplete/failed reads preserve the previous pointer edge state. */
h2_pal_result_t h2_fruit_slice_pointer_init(h2_fruit_slice_pointer_t* pointer,
                                            int32_t width, int32_t height,
                                            h2_fruit_slice_pointer_read_fn read,
                                            void* user);
/** Borrow the Touch PAL until the pointer storage and callback are retired. */
const h2_pal_touch_api_t* h2_fruit_slice_pointer_touch(
    h2_fruit_slice_pointer_t* pointer);
#ifdef __cplusplus
}
#endif
#endif
