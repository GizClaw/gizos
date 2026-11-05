#include "h2_bk_boot_context.h"
#include "h2_bk_h2loader.h"
#include "h2_loader_metadata.h"
#include "h2/pal/core/h2_pal_errors.h"

static const h2_pal_pref_api_t* s_pref;
static const h2_pal_mem_api_t* s_mem;

int h2_bk_h2loader_boot_context_bind(const h2_pal_pref_api_t* pref,
                                     const h2_pal_mem_api_t* mem) {
  if (!pref || !pref->vtable || !mem || !mem->vtable)
    return H2_PAL_ERR_INVALID_ARG;
  if (s_pref && (s_pref != pref || s_mem != mem))
    return H2_PAL_ERR_INVALID_STATE;
  s_pref = pref;
  s_mem = mem;
  return H2_PAL_OK;
}

int h2_bk_h2loader_boot_context_unbind(const h2_pal_pref_api_t* pref,
                                       const h2_pal_mem_api_t* mem) {
  if (!pref || !mem) return H2_PAL_ERR_INVALID_ARG;
  if (s_pref && (s_pref != pref || s_mem != mem))
    return H2_PAL_ERR_INVALID_STATE;
  s_pref = NULL;
  s_mem = NULL;
  return H2_PAL_OK;
}

int h2_bk_boot_context_ready(void) {
  return s_pref && s_mem ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

int h2_bk_boot_loader_window_committed(void) {
  h2_loader_metadata_t metadata;
  int present = 0;
  if (!s_pref || !s_mem) return H2_PAL_ERR_INVALID_STATE;
  const int rc = h2_loader_metadata_read(
      s_pref, s_mem, H2_LOADER_METADATA_SLOT_PARTITION_1, &metadata, &present);
  if (rc != H2_PAL_OK) return rc;
  return present && metadata.valid;
}

int h2_bk_boot_candidate_is_loader(uint64_t* out_image_size) {
  h2_loader_metadata_t metadata;
  int present = 0;
  if (!out_image_size) return H2_PAL_ERR_INVALID_ARG;
  *out_image_size = 0;
  if (!s_pref || !s_mem) return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_loader_metadata_read(
      s_pref, s_mem, H2_LOADER_METADATA_SLOT_PARTITION_2, &metadata, &present);
  if (rc != H2_PAL_OK) return rc;
  if (!present || !metadata.valid) return H2_PAL_ERR_INVALID_STATE;
  *out_image_size = metadata.image_size;
  if (metadata.role == H2_LOADER_IMAGE_ROLE_H2LOADER) return 1;
  return metadata.role == H2_LOADER_IMAGE_ROLE_APP ? 0 : H2_PAL_ERR_FORMAT;
}
