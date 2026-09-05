#include "h2_webrtc_compat_factory.h"

#include "h2_desktop_platform.h"
#include "h2_pion.h"

// Keep the provider cleanup check enabled in optimized test builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static void destroy(void *opaque) {
  h2_pion_t *provider = opaque;
  h2_pion_destroy(&provider);
  assert(provider == NULL);
}

h2_pal_result_t
h2_webrtc_compat_backend_create(h2_webrtc_compat_backend_t *out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  const h2_pion_config_t config = {
      .mem = h2_desktop_platform_default_allocator(),
      .sync = h2_desktop_platform_sync_api(),
      .task = h2_desktop_platform_task_api(),
      .time = h2_desktop_platform_time_api(),
  };
  h2_pion_t *provider = NULL;
  h2_pal_result_t rc = h2_pion_create(&config, &provider);
  if (rc != H2_PAL_OK)
    return rc;
  out->api = h2_pion_webrtc_api(provider);
  out->state = provider;
  out->destroy = destroy;
  out->name = "pion";
  out->supports_turn = 1;
  // The default Pion network configuration does not enable ICE TCP.
  out->supports_ice_tcp = 0;
  return H2_PAL_OK;
}
