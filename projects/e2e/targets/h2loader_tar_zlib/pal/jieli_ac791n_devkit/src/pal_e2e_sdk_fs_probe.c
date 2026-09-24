/* Raw SDK filesystem probes for the O9 audit. This translation unit includes
 * the SDK fs header, whose FILE type conflicts with newlib stdio, so the
 * diagnostic main only receives integer results. */
#include "fs/fs.h"

#include "pal_e2e_sdk_fs_probe.h"

void h2_pal_e2e_sdk_probe_frename_existing(
    const char *native_a, const char *native_b,
    h2_pal_e2e_sdk_rename_probe_t *out) {
  FILE *sdk_a = fopen(native_a, "w+");
  out->create_a = sdk_a != NULL;
  FILE *sdk_b = fopen(native_b, "w+");
  out->create_b = sdk_b != NULL;
  out->close_b = sdk_b == NULL ? -1 : fclose(sdk_b);
  out->rename = sdk_a == NULL ? -1 : frename(sdk_a, "b");
  out->close_a = sdk_a == NULL ? -1 : fclose(sdk_a);
  sdk_a = fopen(native_a, "r");
  out->delete_a = sdk_a == NULL ? -1 : fdelete(sdk_a);
  sdk_b = fopen(native_b, "r");
  out->delete_b = sdk_b == NULL ? -1 : fdelete(sdk_b);
}

void h2_pal_e2e_sdk_probe_fopen_dir_write(
    const char *native_dir, h2_pal_e2e_sdk_open_probe_t *out) {
  FILE *sdk_dir = fopen(native_dir, "w+");
  out->opened = sdk_dir != NULL;
  out->close = sdk_dir == NULL ? -1 : fclose(sdk_dir);
}
