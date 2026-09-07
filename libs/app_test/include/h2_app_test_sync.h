#ifndef H2_APP_TEST_SYNC_H
#define H2_APP_TEST_SYNC_H
#include "h2/pal/os/h2_pal_sync.h"
#include "h2_app_test_fault.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define H2_APP_TEST_SYNC_MUTEXES_MAX 16u
/** Serialized, nonblocking mutex lifecycle fake. Contended lock returns BUSY;
 * no OS synchronization or task scheduling is provided. Use real/libco Sync PAL
 * for concurrent subjects. Controls and observations require quiescent callers.
 */
typedef struct h2_app_test_sync {
  h2_pal_sync_api_t api;
  struct {
    bool active, locked;
  } mutexes[H2_APP_TEST_SYNC_MUTEXES_MAX];
  h2_app_test_fault_t create, destroy, lock, unlock;
} h2_app_test_sync_t;
void h2_app_test_sync_init(h2_app_test_sync_t *sync);
#ifdef __cplusplus
}
#endif
#endif
