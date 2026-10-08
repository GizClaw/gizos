#ifndef FAKE_ESP_WN_MODELS_H
#define FAKE_ESP_WN_MODELS_H
#include <stdint.h>
typedef struct model_iface_data model_iface_data_t;
typedef enum { DET_MODE_90 = 0 } det_mode_t;
typedef enum {
    WAKENET_CHANNEL_VERIFIED = -1, WAKENET_NO_DETECT = 0, WAKENET_DETECTED = 1,
} wakenet_state_t;
typedef struct {
    model_iface_data_t *(*create)(const void *, det_mode_t);
    int (*get_samp_chunksize)(model_iface_data_t *);
    int (*get_samp_rate)(model_iface_data_t *);
    int (*get_channel_num)(model_iface_data_t *);
    wakenet_state_t (*detect)(model_iface_data_t *, int16_t *);
    void (*clean)(model_iface_data_t *);
    void (*destroy)(model_iface_data_t *);
} esp_wn_iface_t;
const esp_wn_iface_t *esp_wn_handle_from_name(const char *name);
#endif
