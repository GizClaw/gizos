#ifndef H2_GIZCLAW_DOWNLOAD_INTERNAL_H
#define H2_GIZCLAW_DOWNLOAD_INTERNAL_H

#include "h2_gizclaw_service_internal.h"

typedef struct h2_gizclaw_download_codec {
  h2_pal_result_t (*metadata)(void *user, h2_gizclaw_rpc_bytes_t payload,
                              uint64_t *out_size);
  int (*write)(void *user, const uint8_t *data, size_t len);
  void (*destroy)(void *user);
  /* Optional route admission at do; failed admission leaves existing routes
   * untouched. received runs once on the downlink task after validated download,
   * then the sink publishes req_sink_done when its work is finished.
   * stop quiesces in-flight sink accesses before request settlement. */
  h2_pal_result_t (*admit)(void *user);
  void (*received)(void *user, h2_gizclaw_req_t *request);
  void (*stop)(void *user);
  bool audio_sink;
} h2_gizclaw_download_codec_t;

/* Copies the encoded input. Takes ownership of user only on success. */
h2_pal_result_t h2_gizclaw_req_create_download_internal(
    h2_gizclaw_service_t *service, uint64_t identity, const void *tag,
    h2_gizclaw_rpc_method_t method, h2_gizclaw_rpc_bytes_t payload,
    uint32_t timeout_ms, const h2_gizclaw_download_codec_t *codec, void *user,
    h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_download_result_internal(const h2_gizclaw_req_t *request,
                                    const void *tag, const void **out_user,
                                    uint64_t *out_received);

#endif
