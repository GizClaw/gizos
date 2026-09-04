#include "h2_gizclaw_download_internal.h"

#include <limits.h>
#include <string.h>

typedef struct download {
  const h2_pal_mem_api_t *allocator;
  const h2_gizclaw_download_codec_t *codec;
  void *user;
  uint64_t expected;
  uint64_t received;
  h2_pal_result_t frame_error;
  bool metadata_seen;
  bool eos_seen;
} download_t;

static int download_frame(void *user,
                          const h2_gizclaw_rpc_stream_event_t *event) {
  download_t *download = user;
  if (download->frame_error != H2_PAL_OK)
    return download->frame_error;
  h2_pal_result_t rc = H2_PAL_OK;
  if (event == NULL || download->eos_seen)
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK && event->has_error)
    rc = h2_gizclaw_rpc_error_result_internal(event->error_code);
  if (rc != H2_PAL_OK)
    return download->frame_error = rc;
  switch (event->kind) {
  case H2_GIZCLAW_RPC_STREAM_RESPONSE:
    if (download->metadata_seen)
      rc = H2_PAL_ERR_FORMAT;
    else {
      rc = download->codec->metadata(download->user, event->result_payload,
                                     &download->expected);
      if (rc == H2_PAL_OK)
        download->metadata_seen = true;
    }
    break;
  case H2_GIZCLAW_RPC_STREAM_DATA:
    if (!download->metadata_seen || download->received > download->expected ||
        event->data.len > download->expected - download->received ||
        (event->data.len != 0u && event->data.data == NULL))
      rc = H2_PAL_ERR_FORMAT;
    else if (event->data.len != 0u) {
      if (download->codec->write != NULL)
        rc = (h2_pal_result_t)download->codec->write(
            download->user, event->data.data, event->data.len);
      if (rc == H2_PAL_OK)
        download->received += event->data.len;
      if (rc == H2_PAL_ERR_WOULD_BLOCK || rc > H2_PAL_OK)
        rc = H2_PAL_ERR_IO;
    }
    break;
  case H2_GIZCLAW_RPC_STREAM_EOS:
    if (!download->metadata_seen || download->received != download->expected)
      rc = H2_PAL_ERR_FORMAT;
    else
      download->eos_seen = true;
    break;
  default:
    rc = H2_PAL_ERR_FORMAT;
    break;
  }
  download->frame_error = rc;
  return rc;
}

static h2_pal_result_t download_admit(void *user) {
  download_t *download = user;
  return download->codec->admit != NULL
             ? download->codec->admit(download->user) : H2_PAL_OK;
}

static void download_received(void *user, h2_gizclaw_req_t *request) {
  download_t *download = user;
  if (download->codec->received != NULL)
    download->codec->received(download->user, request);
  else
    h2_gizclaw_req_sink_done_internal(request, H2_PAL_OK);
}

static void download_stop(void *user) {
  download_t *download = user;
  if (download->codec->stop != NULL)
    download->codec->stop(download->user);
}

static void download_destroy(void *user) {
  download_t *download = user;
  if (download->user != NULL)
    download->codec->destroy(download->user);
  h2_pal_mem_free(download->allocator, download);
}

h2_pal_result_t h2_gizclaw_req_create_download_internal(
    h2_gizclaw_service_t *service, uint64_t identity, const void *tag,
    h2_gizclaw_rpc_method_t method, h2_gizclaw_rpc_bytes_t payload,
    uint32_t timeout_ms, const h2_gizclaw_download_codec_t *codec, void *user,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || out_request == NULL || tag == NULL || method <= 0 ||
      timeout_ms == 0u || timeout_ms > INT32_MAX || codec == NULL ||
      codec->metadata == NULL || codec->destroy == NULL || user == NULL ||
      (codec->audio_sink && codec->write == NULL) ||
      (payload.len > 0u && payload.data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  download_t *download = h2_pal_mem_alloc(allocator, sizeof(*download));
  if (download == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(download, 0, sizeof(*download));
  download->allocator = allocator;
  download->codec = codec;
  download->user = user;
  h2_pal_result_t rc = h2_gizclaw_req_create_sink_stream_internal(
      service, identity, method, tag, payload, timeout_ms, download_frame,
      download_received, download_admit, download_stop, download_destroy,
      download, codec->audio_sink, out_request);
  if (rc != H2_PAL_OK) {
    download->user = NULL;
    download_destroy(download);
  }
  return rc;
}

h2_pal_result_t
h2_gizclaw_download_result_internal(const h2_gizclaw_req_t *request,
                                    const void *tag, const void **out_user,
                                    uint64_t *out_received) {
  if (out_user == NULL || out_received == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_user = NULL;
  *out_received = 0u;
  const void *context;
  h2_pal_result_t rc = h2_gizclaw_req_context_internal(request, tag, &context);
  if (rc == H2_PAL_OK) {
    const download_t *download = context;
    *out_user = download->user;
    *out_received = download->received;
  }
  return rc;
}
