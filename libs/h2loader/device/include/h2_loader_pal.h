#ifndef H2_LOADER_PAL_H
#define H2_LOADER_PAL_H

#include "h2_loader_boot.h"
#include "h2_loader_digest.h"
#include "h2_loader_memory.h"
#include "h2/pal/os/h2_pal_device_identity.h"
#include "h2/pal/os/h2_pal_image.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_loader_pal_config {
  const h2_pal_image_api_t* image;
  const h2_pal_power_api_t* power;
  const h2_pal_time_api_t* time;
  /** Required by current_image_identity, otherwise optional. */
  const h2_pal_mem_api_t* allocator;
  uint32_t loader_partition_id;
  uint32_t app_partition_id;
  /** Positive blocking delay after each nonempty digest update, or zero.
   * A provider used with a positive delay must round it up to a native tick.
   */
  uint32_t digest_yield_ms;
  int digest_feed_watchdog;
  /** Identity domain only. Default STORED; get_info and reads use this same
   * explicit view. Package ImageReader/Writer always use STORED. */
  h2_pal_image_view_t identity_view;
} h2_loader_pal_config_t;

/** Caller-owned adapters. Serialize all users, including serial/BLE commands,
 * for the full operation. Config APIs and their userdata are borrowed for this
 * object's lifetime. A successful writer finish retains the native candidate;
 * neither reinitialization nor a later begin implicitly cancels that candidate.
 */
typedef struct h2_loader_pal {
  h2_loader_pal_config_t config;
  h2_loader_image_reader_api_t image_reader;
  h2_loader_image_writer_api_t image_writer;
  h2_loader_digest_api_t digest;
  h2_loader_sha256_digest_t sha_state;
  h2_loader_digest_api_t sha_callbacks;
  uint64_t image_size;
  uint64_t written;
  uint32_t execution_partition_id;
  int writing;
  /** Cleanup evidence for the legacy void writer-abort callback. The native
   * provider must retain/poison uncertain state when this is not OK. */
  h2_pal_result_t last_abort_result;
} h2_loader_pal_t;

h2_pal_result_t h2_loader_pal_init(h2_loader_pal_t* state,
                                   const h2_loader_pal_config_t* config);

/** Hash exactly the native-reported STORED image length. Unknown native length
 * returns UNSUPPORTED; partition capacity is not substituted for image length.
 * The caller supplies H2 identity metadata and owns any fallback policy.
 */
h2_pal_result_t h2_loader_pal_current_image_identity(
    h2_loader_pal_t* state, h2_loader_image_role_t role, const char* board,
    const char* target, const char* version,
    h2_loader_image_identity_t* out_identity);

/** Legacy H2 wire identity requires exactly six stable device-id bytes. */
h2_pal_result_t h2_loader_pal_device_uid(
    const h2_pal_device_identity_api_t* api,
    char out_uid[H2_LOADER_DEVICE_UID_MAX]);

/** Callback adapter; user is a borrowed const h2_pal_mem_api_t*. */
h2_pal_result_t h2_loader_pal_memory_stats_read(
    void* user, h2_loader_memory_stats_t* out_stats);

/** Callback adapter for evidence-free platforms; user is a borrowed Power API.
 * Platforms requiring persisted native evidence compose their own callback
 * above PAL, preserving H2 evidence binding and Stage ordering.
 */
int h2_loader_pal_confirm_running_image(void* user);

/** Callback adapter; user is a borrowed const h2_pal_fs_api_t*. */
int h2_loader_pal_prepare_file_point(void* user, const char* path);

#ifdef __cplusplus
}
#endif

#endif
