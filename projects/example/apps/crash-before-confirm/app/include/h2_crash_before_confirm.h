#ifndef H2_CRASH_BEFORE_CONFIRM_H
#define H2_CRASH_BEFORE_CONFIRM_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*h2_crash_fn_t)(void *user);

typedef struct h2_crash_before_confirm_config {
    h2_crash_fn_t crash;
    void *crash_user;
} h2_crash_before_confirm_config_t;

int h2_crash_before_confirm_run(
    h2_runtime_t *runtime,
    const h2_crash_before_confirm_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
