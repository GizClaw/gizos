#ifndef PAL_E2E_SDK_FS_PROBE_H
#define PAL_E2E_SDK_FS_PROBE_H

/* Results of raw SDK fopen/frename/fdelete calls; nonzero SDK results are
 * reported as-is, opens report 1 for a handle and 0 for NULL. */
typedef struct h2_pal_e2e_sdk_rename_probe {
  int create_a;
  int create_b;
  int close_b;
  int rename;
  int close_a;
  int delete_a;
  int delete_b;
} h2_pal_e2e_sdk_rename_probe_t;

typedef struct h2_pal_e2e_sdk_open_probe {
  int opened;
  int close;
} h2_pal_e2e_sdk_open_probe_t;

void h2_pal_e2e_sdk_probe_frename_existing(
    const char *native_a, const char *native_b,
    h2_pal_e2e_sdk_rename_probe_t *out);
void h2_pal_e2e_sdk_probe_fopen_dir_write(
    const char *native_dir, h2_pal_e2e_sdk_open_probe_t *out);

#endif
