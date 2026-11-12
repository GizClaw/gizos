#ifndef H2_G99_STORAGE_WORKER_H
#define H2_G99_STORAGE_WORKER_H

#include "h2_g99_storage.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_g99_storage_snapshot {
  uint32_t version;
  bool pending;
  bool simulated;
  h2_g99_storage_volume_t internal;
  h2_g99_storage_volume_t external;
} h2_g99_storage_snapshot_t;

typedef struct h2_g99_storage h2_g99_storage_t;

h2_pal_result_t h2_g99_storage_create(h2_runtime_t* runtime,
                                      const h2_g99_storage_api_t* api,
                                      h2_g99_storage_t** out_storage);
h2_pal_result_t h2_g99_storage_request(h2_g99_storage_t* storage);
h2_pal_result_t h2_g99_storage_get(h2_g99_storage_t* storage,
                                   h2_g99_storage_snapshot_t* out);
/** Failed cleanup retains *storage and all remaining owners for retry. */
h2_pal_result_t h2_g99_storage_destroy(h2_g99_storage_t** storage);

#ifdef __cplusplus
}
#endif

#endif
