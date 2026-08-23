#ifndef H2_APP_TEST_MEMORY_H
#define H2_APP_TEST_MEMORY_H

#include "h2_app_test.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize a synchronous in-process driver for one App adapter.
 *
 * The driver owns headless LVGL platform/core lifecycle for each session.
 * Only one Memory-driver session may be active in a process at a time.
 */
h2_pal_result_t h2_app_test_memory_driver_init(h2_app_test_driver_t *driver,
                                               h2_app_test_app_t app);

/** Close any open session and release driver state; repeated calls are safe. */
void h2_app_test_memory_driver_deinit(h2_app_test_driver_t *driver);

#ifdef __cplusplus
}
#endif

#endif
