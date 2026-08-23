#ifndef H2_PION_H
#define H2_PION_H

#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/application/h2_pal_webrtc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque owner of the Go/Pion WebRTC PAL provider. */
typedef struct h2_pion h2_pion_t;

/** Dependencies borrowed for the complete provider lifetime. */
typedef struct h2_pion_config {
  const h2_pal_mem_api_t *mem;
} h2_pion_config_t;

/**
 * Creates a reusable Go/Pion WebRTC PAL provider.
 *
 * One provider may own multiple peers. Pion worker goroutines enqueue copied
 * events; PAL callbacks run only from the caller of peer_poll().
 */
h2_pal_result_t h2_pion_create(const h2_pion_config_t *config,
                               h2_pion_t **out_provider);

/** Returns a WebRTC PAL API borrowed until h2_pion_destroy(). */
const h2_pal_webrtc_api_t *h2_pion_webrtc_api(h2_pion_t *provider);

/** Closes all peers and destroys the provider. NULL is accepted. */
void h2_pion_destroy(h2_pion_t **provider);

#ifdef __cplusplus
}
#endif

#endif
