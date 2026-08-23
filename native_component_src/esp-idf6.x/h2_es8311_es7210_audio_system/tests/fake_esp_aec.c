#include "fake_esp_aec.h"

#include "esp_heap_caps.h"

#include <stdlib.h>
#include <string.h>

struct aec_handle {
    int active;
};

static struct aec_handle handle;
static aec_config_t captured_config;
static int chunk_size;
static int16_t captured_mic[FAKE_ESP_AEC_MAX_SAMPLES];
static int16_t captured_ref[FAKE_ESP_AEC_MAX_SAMPLES];
static int process_count;
static int destroy_count;

static void require(int condition) {
    if (!condition) {
        abort();
    }
}

void fake_esp_aec_reset(int chunk_samples) {
    require(chunk_samples > 0);
    require(chunk_samples <= FAKE_ESP_AEC_MAX_SAMPLES / 2);
    memset(&handle, 0, sizeof(handle));
    memset(&captured_config, 0, sizeof(captured_config));
    memset(captured_mic, 0, sizeof(captured_mic));
    memset(captured_ref, 0, sizeof(captured_ref));
    chunk_size = chunk_samples;
    process_count = 0;
    destroy_count = 0;
}

const aec_config_t *fake_esp_aec_config(void) {
    return &captured_config;
}

const int16_t *fake_esp_aec_mic(void) {
    return captured_mic;
}

const int16_t *fake_esp_aec_ref(void) {
    return captured_ref;
}

int fake_esp_aec_process_count(void) {
    return process_count;
}

int fake_esp_aec_destroy_count(void) {
    return destroy_count;
}

aec_handle_t *aec_create_from_config(const aec_config_t *config) {
    captured_config = *config;
    handle.active = 1;
    return &handle;
}

int aec_get_chunksize(aec_handle_t *aec) {
    require(aec == &handle);
    require(aec->active);
    return chunk_size;
}

void aec_process(
    aec_handle_t *aec,
    const int16_t *mic,
    const int16_t *ref,
    int16_t *out) {
    require(aec == &handle);
    require(aec->active);
    require(captured_config.mic_num == 2);
    memcpy(
        captured_mic,
        mic,
        (size_t)chunk_size * (size_t)captured_config.mic_num * sizeof(int16_t));
    memcpy(captured_ref, ref, (size_t)chunk_size * sizeof(int16_t));
    for (int sample = 0; sample < chunk_size; ++sample) {
        out[sample] = (int16_t)(
            mic[sample] + mic[chunk_size + sample] - ref[sample]);
    }
    process_count++;
}

void aec_destroy(aec_handle_t *aec) {
    require(aec == &handle);
    require(aec->active);
    aec->active = 0;
    destroy_count++;
}

void *heap_caps_aligned_calloc(
    size_t alignment,
    size_t count,
    size_t size,
    unsigned caps) {
    (void)alignment;
    (void)caps;
    return calloc(count, size);
}

void heap_caps_free(void *memory) {
    free(memory);
}
