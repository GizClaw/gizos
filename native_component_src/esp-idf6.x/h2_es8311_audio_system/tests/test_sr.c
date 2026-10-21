#include "h2_esp_es8311_audio_system.h"
#include "esp_aec.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define EXPECTED_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define EXPECTED_MODE AEC_MODE_VOIP_LOW_COST
#else
#define EXPECTED_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define EXPECTED_MODE AEC_MODE_FD_LOW_COST
#endif

struct aec_handle {
  int unused;
};
static struct aec_handle handle;
static int chunk, live, buffers, call_count, fail_at, allocations;
static void* try_alloc(size_t size) {
  if (++allocations == fail_at) return NULL;
  return calloc(1, size);
}
void* heap_caps_aligned_calloc(size_t alignment, size_t count, size_t size,
                               unsigned caps) {
  assert(alignment == 16 && caps == EXPECTED_CAPS);
  void* p = try_alloc(count * size);
  if (p) ++buffers;
  return p;
}
void heap_caps_free(void* p) {
  if (p) {
    --buffers;
    free(p);
  }
}
aec_handle_t* aec_create_from_config(aec_config_t* c) {
  assert(!live && c->caps == EXPECTED_CAPS);
  assert(c->sample_rate == 16000 && c->filter_length == 4 &&
         c->mode == EXPECTED_MODE);
  if (++allocations == fail_at) return NULL;
  live = 1;
  return &handle;
}
int aec_get_chunksize(const aec_handle_t* h) {
  assert(h == &handle && live);
  return chunk;
}
void aec_process(const aec_handle_t* h, int16_t* mic, int16_t* ref,
                 int16_t* out) {
  assert(h == &handle && live);
  ++call_count;
  for (int i = 0; i < chunk; ++i) out[i] = mic[i] - ref[i];
}
void aec_linear_process(const aec_handle_t* h, int16_t* mic, int16_t* ref,
                        int16_t* out) {
  aec_process(h, mic, ref, out);
}
void aec_destroy(aec_handle_t* h) {
  assert(h == &handle && live);
  live = 0;
}
static h2_esp_es8311_audio_system_config_t config(void) {
  return (h2_esp_es8311_audio_system_config_t){.sample_rate_hz = 16000,
                                               .frame_samples_per_channel = 320,
                                               .raw_channels = 2,
                                               .processed_channels = 1,
                                               .mic_channel_index = 1,
                                               .ref_channel_index = 0,
                                               .enable_aec = 1};
}
int main(void) {
  const int inputs[] = {128, 256, 320, 512};
  const int chunks[] = {128, 256, 320, 512};
  for (unsigned input = 0; input < sizeof(inputs) / sizeof(inputs[0]);
       ++input) {
    const int frame_samples = inputs[input];
    for (unsigned c = 0; c < sizeof(chunks) / sizeof(chunks[0]); ++c) {
      chunk = chunks[c];
      allocations = call_count = 0;
      h2_esp_es8311_audio_system_config_t cfg = config();
      cfg.frame_samples_per_channel = (uint16_t)frame_samples;
      h2_esp_es8311_sr_state_t state = {0};
      if (chunk != frame_samples) {
        assert(h2_esp_es8311_sr_init(&state, &cfg) == H2_AUDIO_ERR_UNSUPPORTED);
        assert(!state.available && !live && !buffers && allocations == 1);
        h2_esp_es8311_sr_deinit(&state);
        continue;
      }
      assert(h2_esp_es8311_sr_init(&state, &cfg) == H2_AUDIO_OK);
      int16_t raw[1024], output[512];
      h2_audio_pcm_format_t rf = {
          .sample_rate_hz = 16000,
          .channels = 2,
          .sample_format = H2_AUDIO_SAMPLE_S16LE,
          .frame_samples_per_channel = (uint16_t)frame_samples};
      h2_audio_pcm_format_t of = rf;
      of.channels = 1;
      h2_audio_frame_t in = h2_audio_frame_for_buffer(raw, sizeof(raw), rf);
      h2_audio_frame_t out =
          h2_audio_frame_for_buffer(output, sizeof(output), of);
      in.bytes = (size_t)frame_samples * 2 * sizeof(int16_t);
      in.samples_per_channel = (uint16_t)frame_samples;
      for (int f = 0; f < 20; ++f) {
        for (int i = 0; i < frame_samples; ++i) {
          raw[2 * i] = 7;
          /* Twenty frames of at most 512 samples stay below INT16_MAX. */
          raw[2 * i + 1] = (int16_t)(f * frame_samples + i + 8);
        }
        assert(h2_esp_es8311_sr_process(&state, &in, &out, 0) == 0);
        assert(out.bytes == (size_t)frame_samples * sizeof(int16_t) &&
               out.samples_per_channel == frame_samples);
        for (int i = 0; i < frame_samples; ++i) {
          assert(output[i] == f * frame_samples + i + 1);
        }
      }
      assert(call_count == 20 && allocations == 4);
      in.samples_per_channel = (uint16_t)(frame_samples - 1);
      assert(h2_esp_es8311_sr_process(&state, &in, &out, 0) ==
                 H2_AUDIO_ERR_UNSUPPORTED &&
             out.bytes == 0 && call_count == 20);
      in.samples_per_channel = (uint16_t)frame_samples;
      in.channels = 1;
      assert(h2_esp_es8311_sr_process(&state, &in, &out, 0) != 0 &&
             out.bytes == 0);
      in.channels = 2;
      in.bytes = 1;
      assert(h2_esp_es8311_sr_process(&state, &in, &out, 0) != 0 &&
             out.bytes == 0);
      in.bytes = (size_t)frame_samples * 2 * sizeof(int16_t);
      h2_esp_es8311_sr_reset(&state);
      for (int i = 0; i < chunk; ++i) assert(state.out_frame[i] == 0);
      h2_esp_es8311_sr_deinit(&state);
      h2_esp_es8311_sr_deinit(&state);
      assert(!live && !buffers);
    }
  }
  for (fail_at = 1; fail_at <= 4; ++fail_at) {
    allocations = 0;
    chunk = 320;
    h2_esp_es8311_sr_state_t state = {0};
    h2_esp_es8311_audio_system_config_t cfg = config();
    assert(h2_esp_es8311_sr_init(&state, &cfg) == H2_AUDIO_ERR_NO_MEMORY);
    h2_esp_es8311_sr_deinit(&state);
    assert(!live && !buffers);
  }
  fail_at = 0;
  chunk = 513;
  h2_esp_es8311_sr_state_t state = {0};
  h2_esp_es8311_audio_system_config_t cfg = config();
  assert(h2_esp_es8311_sr_init(&state, &cfg) == H2_AUDIO_ERR_UNSUPPORTED);
  assert(!live && !buffers);
  cfg.ref_channel_index = cfg.mic_channel_index;
  assert(h2_esp_es8311_sr_init(&state, &cfg) == H2_AUDIO_ERR_UNSUPPORTED);
  cfg = config();
  cfg.sample_rate_hz = 8000;
  assert(h2_esp_es8311_sr_init(&state, &cfg) == H2_AUDIO_ERR_UNSUPPORTED);
  return 0;
}
