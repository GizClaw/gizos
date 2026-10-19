#include "h2_android_net.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_android_tls_internal.h"
#include "h2_posix_pal_core.h"
struct h2_android_net {
  int tls_reference;
};
h2_pal_result_t h2_android_net_create(h2_android_net_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  int rc = h2_android_tls_acquire();
  if (rc != H2_PAL_OK)
    return (h2_pal_result_t)rc;
  h2_android_net_t *owner =
      h2_pal_mem_alloc(h2_android_platform_mem_api(), sizeof(*owner));
  if (!owner) {
    (void)h2_android_tls_release();
    return H2_PAL_ERR_NO_MEMORY;
  }
  owner->tls_reference = 1;
  *out = owner;
  return H2_PAL_OK;
}
const h2_pal_net_api_t *h2_android_net_api(h2_android_net_t *owner) {
  return owner ? h2_posix_net_api() : h2_pal_unsupported_net_api();
}
h2_pal_result_t h2_android_net_destroy(h2_android_net_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  if (!*out)
    return H2_PAL_OK;
  int rc = h2_android_tls_release();
  if (rc != H2_PAL_OK)
    return (h2_pal_result_t)rc;
  h2_pal_mem_free(h2_android_platform_mem_api(), *out);
  *out = NULL;
  return H2_PAL_OK;
}
