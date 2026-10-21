#include "h2_esp_es8311_audio_system.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <string.h>

#if defined(__has_include)
#if __has_include("esp_aec.h")
#include "esp_aec.h"
#define H2_ESP_ES8311_HAVE_DIRECT_AEC 1
#endif
#endif

#if !defined(H2_ESP_ES8311_HAVE_DIRECT_AEC)
#define H2_ESP_ES8311_HAVE_DIRECT_AEC 0
#endif

#define H2_ESP_ES8311_AEC_SAMPLE_RATE 16000u
#define H2_ESP_ES8311_AEC_MIC_COUNT 1u
#define H2_ESP_ES8311_AEC_REF_COUNT 1u
#define H2_ESP_ES8311_AEC_FILTER_LENGTH 4

static const char* TAG = "h2_es8311_aec";

#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define H2_ESP_ES8311_AEC_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define H2_ESP_ES8311_AEC_MODE AEC_MODE_VOIP_LOW_COST
#define H2_ESP_ES8311_AEC_MODE_NAME "voip_low_cost"
#else
#define H2_ESP_ES8311_AEC_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define H2_ESP_ES8311_AEC_MODE AEC_MODE_FD_LOW_COST
#define H2_ESP_ES8311_AEC_MODE_NAME "fd_low_cost"
#endif

static void* aec_calloc(size_t count, size_t size) {
  return heap_caps_aligned_calloc(16u, count, size, H2_ESP_ES8311_AEC_CAPS);
}

static void sr_free_buffers(h2_esp_es8311_sr_state_t* state) {
  if (state == NULL) {
    return;
  }
#if H2_ESP_ES8311_HAVE_DIRECT_AEC
  if (state->aec_handle != NULL) {
    aec_destroy((aec_handle_t*)state->aec_handle);
  }
#endif
  heap_caps_free(state->mic_frame);
  heap_caps_free(state->ref_frame);
  heap_caps_free(state->out_frame);
  state->aec_handle = NULL;
  state->mic_frame = NULL;
  state->ref_frame = NULL;
  state->out_frame = NULL;
  state->available = 0;
  state->frame_samples = 0u;
}

static int validate_config(const h2_esp_es8311_audio_system_config_t* config) {
  if (config == NULL ||
      config->sample_rate_hz != H2_ESP_ES8311_AEC_SAMPLE_RATE ||
      config->frame_samples_per_channel == 0u ||
      config->frame_samples_per_channel >
          H2_ESP_ES8311_AUDIO_SYSTEM_MAX_FRAME_SAMPLES ||
      (config->aec_nlp_level != H2_ESP_ES8311_AEC_NLP_NORMAL &&
       config->aec_nlp_level != H2_ESP_ES8311_AEC_NLP_AGGRESSIVE) ||
      config->processed_channels != 1u || config->raw_channels == 0u ||
      config->raw_channels > H2_ESP_ES8311_AUDIO_SYSTEM_MAX_RAW_CHANNELS ||
      (config->enable_aec &&
       config->mic_channel_index == config->ref_channel_index) ||
      config->mic_channel_index >= config->raw_channels ||
      config->ref_channel_index >= config->raw_channels) {
    return H2_AUDIO_ERR_UNSUPPORTED;
  }
  return H2_AUDIO_OK;
}

int h2_esp_es8311_sr_init(h2_esp_es8311_sr_state_t* state,
                          const h2_esp_es8311_audio_system_config_t* config) {
  if (state == NULL || config == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  memset(state, 0, sizeof(*state));
  state->initialized = 1;

  int rc = validate_config(config);
  if (rc != H2_AUDIO_OK) {
    return rc;
  }
  if (!config->enable_aec) {
    return H2_AUDIO_OK;
  }

#if H2_ESP_ES8311_HAVE_DIRECT_AEC
  aec_config_t aec_config = {
      .mic_num = (int)H2_ESP_ES8311_AEC_MIC_COUNT,
      .ref_num = (int)H2_ESP_ES8311_AEC_REF_COUNT,
      .out_num = 1,
      .filter_length = H2_ESP_ES8311_AEC_FILTER_LENGTH,
      .sample_rate = (int)config->sample_rate_hz,
      .caps = H2_ESP_ES8311_AEC_CAPS,
      .mode = H2_ESP_ES8311_AEC_MODE,
      .nlp_level = config->aec_nlp_level == H2_ESP_ES8311_AEC_NLP_AGGRESSIVE
                       ? AEC_NLP_LEVEL_AGGR
                       : AEC_NLP_LEVEL_NORMAL,
  };
  aec_handle_t* aec_handle = aec_create_from_config(&aec_config);
  if (aec_handle == NULL) {
    ESP_LOGE(TAG, "aec_create_from_config failed");
    return H2_AUDIO_ERR_NO_MEMORY;
  }
  state->aec_handle = aec_handle;

  int chunk_samples = aec_get_chunksize(aec_handle);
  if (chunk_samples <= 0 ||
      (unsigned)chunk_samples > H2_ESP_ES8311_AUDIO_SYSTEM_MAX_FRAME_SAMPLES) {
    ESP_LOGE(TAG, "aec chunk unsupported chunk=%d maximum=%u", chunk_samples,
             H2_ESP_ES8311_AUDIO_SYSTEM_MAX_FRAME_SAMPLES);
    sr_free_buffers(state);
    return H2_AUDIO_ERR_UNSUPPORTED;
  }
  state->frame_samples = (size_t)chunk_samples;
  state->input_samples = config->frame_samples_per_channel;
  state->linear_only = 0;
  state->raw_channels = config->raw_channels;
  state->mic_channel_index = config->mic_channel_index;
  state->ref_channel_index = config->ref_channel_index;

  state->mic_frame =
      (int16_t*)aec_calloc(state->frame_samples, sizeof(int16_t));
  state->ref_frame =
      (int16_t*)aec_calloc(state->frame_samples, sizeof(int16_t));
  state->out_frame =
      (int16_t*)aec_calloc(state->frame_samples, sizeof(int16_t));
  if (state->mic_frame == NULL || state->ref_frame == NULL ||
      state->out_frame == NULL) {
    sr_free_buffers(state);
    return H2_AUDIO_ERR_NO_MEMORY;
  }

  state->available = 1;
  ESP_LOGI(TAG, "direct aec ready frame_samples=%u mode=%s filter=%d nlp=%s",
           (unsigned)state->frame_samples, H2_ESP_ES8311_AEC_MODE_NAME,
           H2_ESP_ES8311_AEC_FILTER_LENGTH,
           config->aec_nlp_level == H2_ESP_ES8311_AEC_NLP_AGGRESSIVE
               ? "aggressive"
               : "normal");
  return H2_AUDIO_OK;
#else
  ESP_LOGE(TAG, "esp_aec.h unavailable; direct AEC is required");
  return H2_AUDIO_ERR_UNSUPPORTED;
#endif
}

int h2_esp_es8311_sr_process(h2_esp_es8311_sr_state_t* state,
                             const h2_audio_frame_t* raw_frame,
                             h2_audio_frame_t* out_frame, uint32_t timeout_ms) {
  (void)timeout_ms;
  if (out_frame != NULL) {
    out_frame->bytes = 0u;
    out_frame->samples_per_channel = 0u;
  }
  if (state == NULL || raw_frame == NULL || out_frame == NULL ||
      raw_frame->data == NULL || out_frame->data == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  if (!state->available || state->aec_handle == NULL ||
      state->mic_frame == NULL || state->ref_frame == NULL ||
      state->out_frame == NULL) {
    return H2_AUDIO_ERR_UNSUPPORTED;
  }
  if (raw_frame->sample_format != H2_AUDIO_SAMPLE_S16LE ||
      out_frame->sample_format != H2_AUDIO_SAMPLE_S16LE ||
      raw_frame->sample_rate_hz != H2_ESP_ES8311_AEC_SAMPLE_RATE ||
      out_frame->sample_rate_hz != H2_ESP_ES8311_AEC_SAMPLE_RATE ||
      out_frame->channels != 1u || raw_frame->channels != state->raw_channels ||
      raw_frame->samples_per_channel != state->input_samples ||
      out_frame->capacity < state->input_samples * sizeof(int16_t)) {
    return H2_AUDIO_ERR_UNSUPPORTED;
  }

#if H2_ESP_ES8311_HAVE_DIRECT_AEC
  const size_t raw_frame_bytes = h2_audio_frame_frame_bytes(raw_frame);
  if (raw_frame_bytes == 0u ||
      raw_frame->bytes < state->input_samples * raw_frame_bytes ||
      raw_frame->capacity < raw_frame->bytes) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }

  const int16_t* raw = (const int16_t*)raw_frame->data;
  int16_t* out = (int16_t*)out_frame->data;
  if (state->input_samples == state->frame_samples) {
    for (size_t i = 0u; i < state->frame_samples; ++i) {
      state->mic_frame[i] =
          raw[i * state->raw_channels + state->mic_channel_index];
      state->ref_frame[i] =
          raw[i * state->raw_channels + state->ref_channel_index];
    }
    if (state->linear_only) {
      aec_linear_process(state->aec_handle, state->mic_frame, state->ref_frame,
                         state->out_frame);
    } else {
      aec_process(state->aec_handle, state->mic_frame, state->ref_frame,
                  state->out_frame);
    }
    memcpy(out, state->out_frame, state->input_samples * sizeof(int16_t));
  } else {
    /* Consume the previous chunk while collecting the next. This keeps
     * latency constant, never drops samples and allocates nothing here. */
    for (size_t i = 0u; i < state->input_samples; ++i) {
      size_t at = state->pending_samples;
      state->mic_frame[at] =
          raw[i * state->raw_channels + state->mic_channel_index];
      state->ref_frame[at] =
          raw[i * state->raw_channels + state->ref_channel_index];
      out[i] = state->out_frame[at];
      if (++state->pending_samples == state->frame_samples) {
        if (state->linear_only) {
          aec_linear_process(state->aec_handle, state->mic_frame,
                             state->ref_frame, state->out_frame);
        } else {
          aec_process(state->aec_handle, state->mic_frame, state->ref_frame,
                      state->out_frame);
        }
        state->pending_samples = 0u;
      }
    }
  }
  out_frame->bytes = state->input_samples * sizeof(int16_t);
  out_frame->samples_per_channel = (uint16_t)state->input_samples;
  return H2_AUDIO_OK;
#else
  return H2_AUDIO_ERR_UNSUPPORTED;
#endif
}

void h2_esp_es8311_sr_reset(h2_esp_es8311_sr_state_t* state) {
  if (state == NULL || !state->available || state->frame_samples == 0u) {
    return;
  }
  state->pending_samples = 0u;
  if (state->mic_frame != NULL) {
    memset(state->mic_frame, 0, state->frame_samples * sizeof(int16_t));
  }
  if (state->ref_frame != NULL) {
    memset(state->ref_frame, 0, state->frame_samples * sizeof(int16_t));
  }
  if (state->out_frame != NULL) {
    memset(state->out_frame, 0, state->frame_samples * sizeof(int16_t));
  }
}

void h2_esp_es8311_sr_deinit(h2_esp_es8311_sr_state_t* state) {
  if (state == NULL) {
    return;
  }
  sr_free_buffers(state);
  memset(state, 0, sizeof(*state));
}
