#ifndef H2_P4_TEST_VIDEO_INIT_H
#define H2_P4_TEST_VIDEO_INIT_H
#include "driver/i2c_master.h"
#define ESP_VIDEO_INIT_FLAGS_MIPI_CSI 1u
#define ESP_VIDEO_INIT_FLAGS_ISP 8u
typedef struct {
  bool init_sccb;
  i2c_master_bus_handle_t i2c_handle;
  uint32_t freq;
} esp_video_init_sccb_config_t;
typedef struct {
  esp_video_init_sccb_config_t sccb_config;
  int reset_pin, pwdn_pin;
  bool dont_init_ldo;
} esp_video_init_csi_config_t;
typedef struct {
  const esp_video_init_csi_config_t* csi;
} esp_video_init_config_t;
esp_err_t esp_video_init_with_flags(const esp_video_init_config_t* config,
                                    uint32_t flags);
esp_err_t esp_video_deinit_with_flags(uint32_t flags);
#endif
