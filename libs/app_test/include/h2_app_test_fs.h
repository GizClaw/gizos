#ifndef H2_APP_TEST_FS_H
#define H2_APP_TEST_FS_H
#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2_app_test_fault.h"
#ifdef __cplusplus
extern "C" {
#endif
#define H2_APP_TEST_FS_FILES_MAX 16u
#define H2_APP_TEST_FS_PATH_MAX 127u
/** In-memory regular files keyed by exact path (no directory normalization).
 * One open handle per path. write/seek are bounded by max_file_bytes (default
 * 1 MiB); sparse gaps read as zero. remove/rename reject open files. mkdir and
 * directory clear are unsupported. Paths/data are copied, never host files.
 * max_read_bytes limits each successful read for partial-read tests
 * (0=unlimited). Configure controls while quiescent; follow fault.h
 * serialization rules. */
typedef struct h2_app_test_fs {
  h2_pal_fs_api_t api;
  const h2_pal_mem_api_t *mem;
  void *implementation;
  size_t max_file_bytes, max_read_bytes;
  uint32_t active_handles;
  h2_app_test_fault_t open, read, write, sync, close, stat, remove, rename;
} h2_app_test_fs_t;
/** Initialize fresh storage, borrowing allocator; INVALID_ARG/NO_MEMORY on
 * failure. */
h2_pal_result_t h2_app_test_fs_init(h2_app_test_fs_t *fs,
                                    const h2_pal_mem_api_t *mem);
/** Open handles return INVALID_STATE; NULL/repeated deinit succeeds. */
h2_pal_result_t h2_app_test_fs_deinit(h2_app_test_fs_t *fs);

#ifdef __cplusplus
}
#endif
#endif
