#ifndef H2_AAC_E2E_VECTORS_H
#define H2_AAC_E2E_VECTORS_H

#include <stddef.h>
#include <stdint.h>

typedef struct h2_aac_e2e_packet {
    const uint8_t *data;
    size_t size;
} h2_aac_e2e_packet_t;

typedef struct h2_aac_e2e_vector {
    const char *name;
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t audio_specific_config[2];
    uint32_t tone_hz[2];
    const h2_aac_e2e_packet_t *packets;
    size_t packet_count;
} h2_aac_e2e_vector_t;

extern const h2_aac_e2e_vector_t h2_aac_e2e_mono;
extern const h2_aac_e2e_vector_t h2_aac_e2e_stereo;

#endif
