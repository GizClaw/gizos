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

#ifdef __cplusplus
}
#endif
#endif
