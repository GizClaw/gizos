#include "h2_app_test_audio_fake.h"
#include <string.h>
typedef struct track {
  h2_pal_audio_track_t api;
  h2_app_test_audio_fake_t *owner;
  bool active;
  h2_audio_pcm_format_t format;
  uint32_t volume;
} track_t;
typedef struct store {
  track_t tracks[H2_APP_TEST_AUDIO_FAKE_TRACKS_MAX];
} store_t;
static bool format_valid(const h2_audio_pcm_format_t *f) {
  return f->sample_rate_hz && f->frame_samples_per_channel &&
         h2_audio_pcm_frame_bytes(f);
}
static int info(void *u, h2_audio_info_t *out) {
  h2_app_test_audio_fake_t *a = u;
  memset(out, 0, sizeof(*out));
  int rc = h2_app_test_fault_take(&a->get_info);
  if (!rc)
    *out = a->info;
  return rc;
}
static int start_mic(void *u) {
  h2_app_test_audio_fake_t *a = u;
  if (a->mic_active)
    return H2_PAL_ERR_INVALID_STATE;
  if (!a->info.available || !a->info.mic_supported)
    return H2_PAL_ERR_UNSUPPORTED;
  if (!format_valid(&a->info.mic_format))
    return H2_PAL_ERR_FORMAT;
  int rc = h2_app_test_fault_take(&a->start_mic);
  if (!rc)
    a->mic_active = true;
  return rc;
}
static int stop_mic(void *u) {
  h2_app_test_audio_fake_t *a = u;
  int rc = h2_app_test_fault_take(&a->stop_mic);
  if (!rc)
    a->mic_active = false;
  return rc;
}
static int start_speaker(void *u) {
  h2_app_test_audio_fake_t *a = u;
  if (!a->info.available || !a->info.playback_supported)
    return H2_PAL_ERR_UNSUPPORTED;
  int rc = h2_app_test_fault_take(&a->start_speaker);
  if (!rc)
    a->speaker_active = true;
  return rc;
}
static int stop_speaker(void *u) {
  h2_app_test_audio_fake_t *a = u;
  if (a->active_tracks)
    return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_app_test_fault_take(&a->stop_speaker);
  if (!rc)
    a->speaker_active = false;
  return rc;
}
static int read_mic(void *u, h2_audio_frame_t *out, uint32_t timeout) {
  h2_app_test_audio_fake_t *a = u;
  out->bytes = 0;
  if (!a->mic_active)
    return H2_PAL_ERR_INVALID_STATE;
  if (!format_valid(&a->info.mic_format))
    return H2_PAL_ERR_FORMAT;
  size_t bytes = h2_audio_pcm_frame_bytes(&a->info.mic_format) *
                 a->info.mic_format.frame_samples_per_channel;
  if (out->capacity < bytes)
    return H2_PAL_ERR_NO_SPACE;
  a->last_timeout_ms = timeout;
  int rc = h2_app_test_fault_take(&a->read_mic);
  if (rc)
    return rc;
  *out =
      h2_audio_frame_for_buffer(out->data, out->capacity, a->info.mic_format);
  memset(out->data, 0, bytes);
  out->bytes = bytes;
  return 0;
}
static int get_volume(void *u, uint32_t *out) {
  *out = ((h2_app_test_audio_fake_t *)u)->volume_percent;
  return 0;
}
static int set_volume(void *u, uint32_t percent) {
  h2_app_test_audio_fake_t *a = u;
  if (percent > 100u)
    return H2_PAL_ERR_INVALID_ARG;
  a->last_volume_percent = percent;
  int rc = h2_app_test_fault_take(&a->set_volume);
  if (!rc)
    a->volume_percent = percent;
  return rc;
}
static int write_track(h2_pal_audio_track_t *api, const h2_audio_frame_t *f,
                       uint32_t timeout) {
  track_t *t = api->user;
  h2_app_test_audio_fake_t *a = t->owner;
  if (!t->active)
    return H2_PAL_ERR_INVALID_STATE;
  if (f->bytes > f->capacity || f->sample_rate_hz != t->format.sample_rate_hz ||
      f->channels != t->format.channels ||
      f->sample_format != t->format.sample_format ||
      f->bytes % h2_audio_pcm_frame_bytes(&t->format))
    return H2_PAL_ERR_FORMAT;
  a->last_timeout_ms = timeout;
  int rc = h2_app_test_fault_take(&a->write_track);
  if (!rc)
    a->playback_bytes = f->bytes > UINT64_MAX - a->playback_bytes
                            ? UINT64_MAX
                            : a->playback_bytes + f->bytes;
  return rc;
}
static int drain_track(h2_pal_audio_track_t *api, uint32_t timeout) {
  track_t *t = api->user;
  if (!t->active)
    return H2_PAL_ERR_INVALID_STATE;
  t->owner->last_timeout_ms = timeout;
  return h2_app_test_fault_take(&t->owner->drain_track);
}
static int close_track(h2_pal_audio_track_t *api) {
  track_t *t = api->user;
  if (!t->active)
    return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_app_test_fault_take(&t->owner->close_track);
  if (!rc) {
    t->active = false;
    --t->owner->active_tracks;
  }
  return rc;
}
static int get_track_volume(h2_pal_audio_track_t *api, uint32_t *out) {
  track_t *t = api->user;
  *out = 0;
  if (!t->active)
    return H2_PAL_ERR_INVALID_STATE;
  *out = t->volume;
  return 0;
}
static int set_track_volume(h2_pal_audio_track_t *api, uint32_t value) {
  track_t *t = api->user;
  if (!t->active)
    return H2_PAL_ERR_INVALID_STATE;
  t->volume = value;
  return 0;
}
static int create_track(void *u, const h2_audio_track_config_t *c,
                        h2_pal_audio_track_t **out) {
  h2_app_test_audio_fake_t *a = u;
  *out = NULL;
  if (!a->speaker_active)
    return H2_PAL_ERR_INVALID_STATE;
  if (!format_valid(&c->format))
    return H2_PAL_ERR_FORMAT;
  int rc = h2_app_test_fault_take(&a->create_track);
  if (rc)
    return rc;
  if (a->active_tracks >= H2_APP_TEST_AUDIO_FAKE_TRACKS_MAX ||
      a->active_tracks >= a->info.max_tracks)
    return H2_PAL_ERR_NO_SPACE;
  store_t *s = a->implementation;
  for (size_t i = 0; i < H2_APP_TEST_AUDIO_FAKE_TRACKS_MAX; ++i) {
    track_t *t = &s->tracks[i];
    if (t->active)
      continue;
    t->owner = a;
    t->active = true;
    t->format = c->format;
    t->volume = c->volume_factor_milli;
    t->api = (h2_pal_audio_track_t){.user = t,
                                    .audio = &a->api,
                                    .write = write_track,
                                    .close = close_track,
                                    .drain = drain_track,
                                    .get_volume_factor = get_track_volume,
                                    .set_volume_factor = set_track_volume};
    ++a->active_tracks;
    *out = &t->api;
    return 0;
  }
  return H2_PAL_ERR_NO_SPACE;
}
static const h2_pal_audio_vtable_t vtable = {
    .get_info = info,
    .start_mic = start_mic,
    .stop_mic = stop_mic,
    .mic_read = read_mic,
    .start_speaker = start_speaker,
    .stop_speaker = stop_speaker,
    .create_track = create_track,
    .get_speaker_volume_percent = get_volume,
    .set_speaker_volume_percent = set_volume};
int h2_app_test_audio_fake_init(h2_app_test_audio_fake_t *a,
                                const h2_pal_mem_api_t *m) {
  if (!a)
    return H2_PAL_ERR_INVALID_ARG;
  memset(a, 0, sizeof(*a));
  if (!m || !m->vtable || !m->vtable->alloc || !m->vtable->free)
    return H2_PAL_ERR_INVALID_ARG;
  a->implementation = h2_pal_mem_alloc(m, sizeof(store_t));
  if (!a->implementation)
    return H2_PAL_ERR_NO_MEMORY;
  memset(a->implementation, 0, sizeof(store_t));
  a->mem = m;
  a->api = (h2_pal_audio_api_t){a, &vtable};
  a->info = (h2_audio_info_t){
      .available = 1,
      .mic_supported = 1,
      .playback_supported = 1,
      .mic_format = {16000u, 320u, 1u, H2_AUDIO_SAMPLE_S16LE},
      .playback_format = {16000u, 320u, 1u, H2_AUDIO_SAMPLE_S16LE},
      .max_tracks = H2_APP_TEST_AUDIO_FAKE_TRACKS_MAX};
  return 0;
}
int h2_app_test_audio_fake_deinit(h2_app_test_audio_fake_t *a) {
  if (!a || !a->implementation)
    return 0;
  if (a->mic_active || a->speaker_active || a->active_tracks)
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_mem_free(a->mem, a->implementation);
  memset(a, 0, sizeof(*a));
  return 0;
}
