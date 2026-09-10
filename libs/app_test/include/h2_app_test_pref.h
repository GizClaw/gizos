#ifndef H2_APP_TEST_PREF_H
#define H2_APP_TEST_PREF_H
#include "h2/pal/os/h2_pal_pref.h"
#include "h2_app_test_fault.h"
#ifdef __cplusplus
extern "C" {
#endif
#define H2_APP_TEST_PREF_NAMESPACES_MAX 8u
#define H2_APP_TEST_PREF_ENTRIES_MAX 64u
#define H2_APP_TEST_PREF_NAME_MAX 63u
#define H2_APP_TEST_PREF_VALUE_MAX 1024u
#define H2_APP_TEST_PREF_HANDLES_MAX 16u
/** Transactional in-memory preferences. One writer per namespace; readers see
 * committed data, writers see staged data. Failed commit retains staging for
 * retry, close discards it. Values are typed and copied; get_blob/get_string
 * allocate through the caller's allocator. Iteration is unsupported.
 * Fields are scenario controls/evidence; follow fault.h serialization rules. */
typedef struct h2_app_test_pref {
  h2_pal_pref_api_t api;
  const h2_pal_mem_api_t *mem;
  void *implementation;
  h2_app_test_fault_t open, read, write, commit;
  /** Empty filters match all commits; key filter matches a staged change. */
  char commit_namespace[H2_APP_TEST_PREF_NAME_MAX + 1u];
  char commit_key[H2_APP_TEST_PREF_NAME_MAX + 1u];
  uint32_t active_handles;
} h2_app_test_pref_t;
/** Initialize fresh caller storage, borrowing allocator until deinit. Returns
 * INVALID_ARG/NO_MEMORY. Do not reinitialize a live instance. */
h2_pal_result_t h2_app_test_pref_init(h2_app_test_pref_t *pref,
                                      const h2_pal_mem_api_t *mem);
/** Release all data. Open handles return INVALID_STATE; NULL/repeated deinit
 * OK. */
h2_pal_result_t h2_app_test_pref_deinit(h2_app_test_pref_t *pref);

#ifdef __cplusplus
}
#endif
#endif
