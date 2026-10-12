#ifndef H2_MOSAICO_PREF_FIXTURE_H
#define H2_MOSAICO_PREF_FIXTURE_H

#include "h2/pal/os/h2_pal_pref.h"
#include <stdint.h>
#include <string.h>

/* These are the portable Pref suite's two private namespaces. The artifact
 * identity belongs to its control namespace, outside the tested data registry.
 * A new image starts a fresh run; ordinary boots retain seed/verify/clean state.
 * Never erase the provider or any device configuration namespace. */
static inline int h2_mosaico_pref_fixture_prepare(
    const h2_pal_pref_api_t *pref, const h2_pal_mem_api_t *mem,
    const uint8_t image_id[32], int *out_reset) {
  h2_pal_pref_namespace_t *control = NULL, *data = NULL;
  void *saved = NULL;
  size_t saved_size = 0u;
  int reset = 0;
  int rc;
  if (pref == NULL || mem == NULL || mem->vtable == NULL ||
      mem->vtable->free == NULL || image_id == NULL || out_reset == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_reset = 0;
  rc = h2_pal_pref_open(pref, "h2e2eprefctl", H2_PAL_PREF_OPEN_READ_WRITE,
                        &control);
  if (rc == H2_PAL_OK) {
    rc = control->get_blob(control, mem, "image", &saved, &saved_size);
    if (rc == H2_PAL_ERR_NOT_FOUND) {
      reset = 1;
      rc = H2_PAL_OK;
    } else if (rc == H2_PAL_OK) {
      if (saved == NULL || saved_size != 32u) rc = H2_PAL_ERR_IO;
      else reset = memcmp(saved, image_id, 32u) != 0;
    }
  }
  h2_pal_mem_free(mem, saved);
  if (rc == H2_PAL_OK && reset)
    rc = h2_pal_pref_open(pref, "h2e2eprefdata", H2_PAL_PREF_OPEN_READ_WRITE,
                          &data);
  if (rc == H2_PAL_OK && reset) rc = data->clear(data);
  if (rc == H2_PAL_OK && reset) rc = data->commit(data);
  if (data != NULL) {
    int close_rc = data->close(data);
    if (rc == H2_PAL_OK) rc = close_rc;
  }
  /* Publish the new identity last: a failed data reset cannot be mistaken for
   * a prepared run on the next boot. A missing phase then selects seed. */
  if (rc == H2_PAL_OK && reset) rc = control->clear(control);
  if (rc == H2_PAL_OK && reset)
    rc = control->set_blob(control, "image", image_id, 32u);
  if (rc == H2_PAL_OK && reset) rc = control->commit(control);
  if (control != NULL) {
    int close_rc = control->close(control);
    if (rc == H2_PAL_OK) rc = close_rc;
  }
  if (rc == H2_PAL_OK) *out_reset = reset;
  return rc;
}

#endif
