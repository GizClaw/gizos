#ifndef H2_PAL_DISK_E2E_H
#define H2_PAL_DISK_E2E_H

#include "h2/pal/os/h2_pal_disk.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_pal_disk_e2e h2_pal_disk_e2e_t;

#define H2_PAL_DISK_E2E_CONTRACT_VERSION 1u

typedef enum h2_pal_disk_e2e_case {
#define H2_PAL_DISK_CASE(symbol, id) H2_PAL_DISK_E2E_CASE_##symbol,
#include "h2_pal_disk_cases.inc"
#undef H2_PAL_DISK_CASE
  H2_PAL_DISK_E2E_CASE_COUNT
} h2_pal_disk_e2e_case_t;

typedef enum h2_pal_disk_e2e_status {
  H2_PAL_DISK_E2E_NOT_RUN,
  H2_PAL_DISK_E2E_PASS,
  H2_PAL_DISK_E2E_FAIL,
  H2_PAL_DISK_E2E_BLOCKED
} h2_pal_disk_e2e_status_t;

/** @brief Stable case identity; returns NULL for an invalid index. */
const char* h2_pal_disk_e2e_case_name(h2_pal_disk_e2e_case_t index);

enum {
  H2_PAL_DISK_E2E_ERR_DATA = -1001,
  H2_PAL_DISK_E2E_ERR_CORE = -1002,
  H2_PAL_DISK_E2E_ERR_REGION = -1003,
  H2_PAL_DISK_E2E_ERR_INCOMPLETE = -1005,
  H2_PAL_DISK_E2E_ERR_ERASE = -1006,
};

typedef enum h2_pal_disk_e2e_stage {
  H2_PAL_DISK_E2E_STAGE_WAIT,
  H2_PAL_DISK_E2E_STAGE_WRITE,
  H2_PAL_DISK_E2E_STAGE_VERIFY,
  H2_PAL_DISK_E2E_STAGE_READ,
  H2_PAL_DISK_E2E_STAGE_DONE,
} h2_pal_disk_e2e_stage_t;

/** @brief Snapshot; worker index 0 is the writer, index 1 the reader.
 * A zero rc is a verdict only after run returns successfully. During progress,
 * counters may advance independently. Without a core observer cores_verified
 * stays false: concurrent tasks alone do not establish cross-core execution.
 */
typedef struct h2_pal_disk_e2e_result {
  int rc;
  int cleanup_rc;
  int output_rc;
  uint32_t rounds[2];
  uint32_t stage[2];
  uint32_t core[2];
  int cores_verified;
  int workers_joined;
  uint32_t passed, failed, blocked;
  h2_pal_disk_e2e_case_t current_case;
  struct {
    h2_pal_disk_e2e_status_t status;
    int rc;
  } cases[H2_PAL_DISK_E2E_CASE_COUNT];
} h2_pal_disk_e2e_result_t;

/** @brief Borrowed providers and callbacks; keep them alive until destroy
 * succeeds. Disk must allow concurrent calls to distinct erase blocks. Only
 * the first two blocks of the nonbootable partition named pal_disk are
 * used; that entire partition must be exclusively reserved for this App.
 *
 * Zero rounds/deadline_ms/ready_ms select the App defaults. current_core is
 * optional, called by each worker, and must be nonblocking. When supplied,
 * expected_core must contain two distinct native core IDs.
 *
 * emit is required, synchronous, called only by the controller, and borrows
 * its snapshot for the call. It must return an error on incomplete output.
 * The "case" phase identifies one terminal registry entry in current_case.
 * Every case is reported once; dependency failures leave blocked entries.
 * Callbacks must not reenter this instance.
 */
typedef struct h2_pal_disk_e2e_config {
  const h2_pal_disk_api_t* disk;
  const h2_pal_task_api_t* task;
  const h2_pal_time_api_t* time;
  const h2_pal_mem_api_t* mem;
  uint32_t partition_id;
  uint32_t rounds;
  uint32_t deadline_ms;
  uint32_t ready_ms;
  uint32_t (*current_core)(void* user);
  void* core_user;
  uint32_t expected_core[2];
  int (*emit)(void* user, const char* phase,
              const h2_pal_disk_e2e_result_t* result);
  void* emit_user;
} h2_pal_disk_e2e_config_t;

/** @brief Allocate a one-shot App instance and validate geometry without
 * writing. On error, *out is NULL. The caller owns the returned instance.
 */
int h2_pal_disk_e2e_create(const h2_pal_disk_e2e_config_t* config,
                           h2_pal_disk_e2e_t** out);

/** @brief Run sequential control, then concurrent erase/write/verify and read.
 * Call once, on a controller task, never concurrently with destroy. Sequential
 * control and cleanup use synchronous Disk calls; their duration is bounded
 * only by the provider. The concurrent deadline requests stop but cannot
 * cancel a blocked Disk call. Such a failure retains worker state and buffers.
 * Success requires complete counts, joined tasks, verified erase and output.
 * Provider errors propagate; run failure never authorizes freeing dependencies.
 */
int h2_pal_disk_e2e_run(h2_pal_disk_e2e_t* app,
                        h2_pal_disk_e2e_result_t* result);

/** @brief Stop, join completed workers, clean scratch and free the instance.
 * NULL is harmless. BUSY means a worker is still active: nothing it borrows is
 * freed or erased. Join/cleanup errors also retain *app for retry. Join is
 * called only after worker completion, but its final teardown is synchronous.
 * On success *app becomes NULL. Call from the owning controller only.
 */
int h2_pal_disk_e2e_destroy(h2_pal_disk_e2e_t** app);

#ifdef __cplusplus
}
#endif
#endif
