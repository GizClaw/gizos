#include "h2_gizclaw_download_internal.h"
#include "h2_gizclaw_ogg_opus_internal.h"
#include "h2_gizclaw_workspace.h"
#include "payload/workspace.pb.h"
#include "pb_decode.h"

#include <string.h>

struct h2_gizclaw_audio_play {
  h2_gizclaw_service_t *service;
  const h2_pal_mem_api_t *allocator;
  char workspace[H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES + 1];
  char history[H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES + 1];
  uint8_t *compressed;
  size_t size, received;
  atomic_bool ready;
  atomic_int result;
  h2_gizclaw_req_t *request; /* Published by ready; pinned until play_stop. */
  size_t refs; /* Service mutex, pins downlink accesses during stop. */
  h2_gizclaw_ogg_opus_t *decoder;
  uint8_t pcm[H2_GIZCLAW_OGG_OPUS_PCM_BYTES];
  size_t pcm_len, pcm_offset;
};
typedef struct h2_gizclaw_audio_play audio_play_t;
static const char audio_play_tag;

static h2_pal_result_t play_admit(void *user) {
  audio_play_t *play = user;
  h2_gizclaw_service_t *service = play->service;
  const h2_pal_sync_api_t *sync = service->config.sync;
  h2_pal_result_t rc = h2_pal_mutex_lock(sync, service->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_track_t *track = atomic_load(&service->pcm_track);
  if (service->audio_play != NULL ||
      atomic_load(&service->media_request) != NULL)
    rc = H2_PAL_ERR_BUSY;
  else if (service->pcm_track_unsetting || track == NULL ||
           track->vtable == NULL || track->vtable->write == NULL)
    rc = H2_PAL_ERR_INVALID_STATE;
  else
    service->audio_play = play;
  (void)h2_pal_mutex_unlock(sync, service->mutex);
  return rc;
}

typedef struct matched_text {
  const char *expected;
  char value[64];
  bool seen;
} matched_text_t;

static bool read_text(pb_istream_t *stream, const pb_field_t *field,
                      void **arg) {
  (void)field;
  matched_text_t *text = *arg;
  if (text->seen)
    return false;
  text->seen = true;
  if (text->expected != NULL) {
    size_t len = strlen(text->expected);
    if (stream->bytes_left != len)
      return false;
    for (size_t i = 0; i < len; ++i) {
      uint8_t byte;
      if (!pb_read(stream, &byte, 1) || byte != (uint8_t)text->expected[i])
        return false;
    }
    return true;
  }
  size_t len = stream->bytes_left;
  if (len >= sizeof(text->value) ||
      !pb_read(stream, (pb_byte_t *)text->value, len) ||
      memchr(text->value, 0, len) != NULL)
    return false;
  text->value[len] = 0;
  return true;
}

static h2_pal_result_t play_metadata(void *user, h2_gizclaw_rpc_bytes_t payload,
                                     uint64_t *out_size) {
  audio_play_t *play = user;
  if (payload.len != 0 && payload.data == NULL)
    return H2_PAL_ERR_FORMAT;
  gizclaw_rpc_v1_WorkspaceHistoryAudioDownloadResponse message =
      gizclaw_rpc_v1_WorkspaceHistoryAudioDownloadResponse_init_zero;
  matched_text_t workspace = {.expected = play->workspace};
  matched_text_t history = {.expected = play->history};
  matched_text_t mime = {0};
  message.workspace_name =
      (pb_callback_t){.funcs.decode = read_text, .arg = &workspace};
  message.history_name =
      (pb_callback_t){.funcs.decode = read_text, .arg = &history};
  message.mime_type = (pb_callback_t){.funcs.decode = read_text, .arg = &mime};
  pb_istream_t input = pb_istream_from_buffer(payload.data, payload.len);
  if (!pb_decode(&input,
                 gizclaw_rpc_v1_WorkspaceHistoryAudioDownloadResponse_fields,
                 &message) ||
      !workspace.seen || !history.seen || !mime.seen ||
      message.size_bytes <= 0 || (uint64_t)message.size_bytes > SIZE_MAX)
    return H2_PAL_ERR_FORMAT;
  if (strcmp(mime.value, "audio/ogg") != 0 &&
      strcmp(mime.value, "audio/ogg; codecs=opus") != 0)
    return H2_PAL_ERR_UNSUPPORTED;
  play->size = (size_t)message.size_bytes;
  play->compressed = h2_pal_mem_alloc(play->allocator, play->size);
  if (play->compressed == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  *out_size = play->size;
  return H2_PAL_OK;
}

static int play_write(void *user, const uint8_t *data, size_t len) {
  audio_play_t *play = user;
  if (play->received > play->size || len > play->size - play->received)
    return H2_PAL_ERR_FORMAT;
  memcpy(play->compressed + play->received, data, len);
  play->received += len;
  return H2_PAL_OK;
}

static void play_received(void *user, h2_gizclaw_req_t *request) {
  audio_play_t *play = user;
  /* Publish the immutable compressed body to the decoder task only after the
   * download runner has verified metadata, byte count and protocol EOS. */
  play->request = request;
  atomic_store_explicit(&play->ready, true, memory_order_release);
}

static void play_stop(void *user) {
  audio_play_t *play = user;
  h2_gizclaw_service_t *service = play->service;
  const h2_pal_sync_api_t *sync = service->config.sync;
  (void)h2_pal_mutex_lock(sync, service->mutex);
  if (service->audio_play == play)
    service->audio_play = NULL;
  while (play->refs != 0)
    (void)h2_pal_cond_wait(sync, service->progress_cond, service->mutex,
                           H2_PAL_SYNC_WAIT_FOREVER);
  (void)h2_pal_mutex_unlock(sync, service->mutex);
}

static void play_destroy(void *user) {
  audio_play_t *play = user;
  h2_gizclaw_ogg_opus_destroy(play->decoder);
  h2_pal_mem_free(play->allocator, play->compressed);
  h2_pal_mem_free(play->allocator, play);
}

static h2_pal_result_t play_step(audio_play_t *play) {
  if (!atomic_load_explicit(&play->ready, memory_order_acquire))
    return H2_PAL_ERR_WOULD_BLOCK;
  h2_pal_result_t rc;
  if (play->decoder == NULL) {
    rc = h2_gizclaw_ogg_opus_create(play->allocator, play->compressed,
                                    play->size, &play->decoder);
    if (rc != H2_PAL_OK)
      return rc;
  }
  if (play->pcm_offset == play->pcm_len) {
    play->pcm_offset = 0;
    rc = h2_gizclaw_ogg_opus_next(play->decoder, play->pcm, sizeof(play->pcm),
                                  &play->pcm_len);
    if (rc == H2_PAL_EXIT)
      return H2_PAL_OK;
    if (rc != H2_PAL_OK)
      return rc;
  }
  size_t len = play->pcm_len - play->pcm_offset;
  if (len != 0) {
    if (len > 640)
      len = 640;
    rc = h2_gizclaw_service_pcm_write_internal(
        play->service, play->pcm + play->pcm_offset, len);
    if (rc == H2_PAL_OK)
      play->pcm_offset += len;
    else if (rc != H2_PAL_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_TIMEOUT)
      return rc < 0 ? rc : H2_PAL_ERR_IO;
  }
  return H2_PAL_ERR_WOULD_BLOCK;
}

void h2_gizclaw_audio_play_downlink_step_internal(
    h2_gizclaw_service_t *service) {
  const h2_pal_sync_api_t *sync = service->config.sync;
  if (h2_pal_mutex_lock(sync, service->mutex) != H2_PAL_OK)
    return;
  audio_play_t *play = service->audio_play;
  if (play != NULL)
    ++play->refs;
  (void)h2_pal_mutex_unlock(sync, service->mutex);
  if (play == NULL)
    return;
  if (atomic_load(&play->result) == H2_PAL_ERR_WOULD_BLOCK) {
    h2_pal_result_t rc = play_step(play);
    if (rc != H2_PAL_ERR_WOULD_BLOCK) {
      atomic_store_explicit(&play->result, rc, memory_order_release);
      h2_gizclaw_req_sink_done_internal(play->request, rc);
    }
  }
  (void)h2_pal_mutex_lock(sync, service->mutex);
  --play->refs;
  (void)h2_pal_cond_broadcast(sync, service->progress_cond);
  (void)h2_pal_mutex_unlock(sync, service->mutex);
}

h2_pal_result_t h2_gizclaw_audio_play_create_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t workspace, h2_gizclaw_str_t history,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  audio_play_t *play = h2_pal_mem_alloc(allocator, sizeof(*play));
  if (play == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(play, 0, sizeof(*play));
  play->service = service;
  play->allocator = allocator;
  memcpy(play->workspace, workspace.data, workspace.len);
  memcpy(play->history, history.data, history.len);
  atomic_init(&play->ready, false);
  atomic_init(&play->result, H2_PAL_ERR_WOULD_BLOCK);
  static const h2_gizclaw_download_codec_t codec = {.metadata = play_metadata,
                                                    .write = play_write,
                                                    .destroy = play_destroy,
                                                    .admit = play_admit,
                                                    .received = play_received,
                                                    .stop = play_stop,
                                                    .audio_sink = true};
  h2_pal_result_t rc = h2_gizclaw_req_create_download_internal(
      service, identity, &audio_play_tag,
      H2_GIZCLAW_RPC_SERVER_WORKSPACE_HISTORY_AUDIO_DOWNLOAD, payload,
      timeout_ms, &codec, play, out_request);
  if (rc != H2_PAL_OK)
    play_destroy(play);
  return rc;
}
