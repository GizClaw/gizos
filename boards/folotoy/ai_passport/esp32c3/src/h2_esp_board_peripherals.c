#include "h2_esp_board_private.h"
#include "h2_cw2017.h"
#include "h2_esp_adc_oneshot.h"
#include "h2_esp_es8311_audio_system.h"
#include "h2_esp_lazy_audio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

enum { DOWN = 101, UP = 102, OK = 103, KEYS = 201, BATTERY = 401 };
enum {
  AUDIO_FRAME_SAMPLES = 512,
  AUDIO_DMA_FRAME_SAMPLES = 128,
  AUDIO_DMA_DESCRIPTORS = 5,
};
/* IDF may discard a partial DMA descriptor when the RX/TX queue backs up. */
_Static_assert(AUDIO_FRAME_SAMPLES % AUDIO_DMA_FRAME_SAMPLES == 0,
               "Native audio frames must consume complete DMA descriptors");
_Static_assert(AUDIO_DMA_DESCRIPTORS* AUDIO_DMA_FRAME_SAMPLES == 640,
               "Keep the board's 40 ms DMA payload budget");
static SemaphoreHandle_t mutex;
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t gauge;
static h2_esp_adc_oneshot_t* adc;
static h2_esp_adc_radio_button_state_t keys;
static h2_esp_es8311_audio_system_t audio;
static h2_esp_lazy_audio_t lazy_audio;
static int audio_ready;
static int audio_aec_enabled = 1;

static int lock(void) {
  return mutex != NULL && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE
             ? 0
             : H2_PAL_ERR_INVALID_STATE;
}

static int init_bus(void) {
  if (bus != NULL) return 0;
  const i2c_master_bus_config_t config = {.i2c_port = 0,
                                          .sda_io_num = 10,
                                          .scl_io_num = 7,
                                          .clk_source = I2C_CLK_SRC_DEFAULT,
                                          .glitch_ignore_cnt = 7,
                                          .flags.enable_internal_pullup = true};
  return i2c_new_master_bus(&config, &bus) == ESP_OK ? 0 : H2_PAL_ERR_IO;
}

static h2_pal_audio_t* resolve_audio(void* user) {
  (void)user;
  if (lock() != 0) return NULL;
  int rc = 0;
  if (!audio_ready) {
    rc = init_bus();
    const h2_esp_es8311_audio_system_config_t config = {
        .sample_rate_hz = 16000,
        .frame_samples_per_channel = AUDIO_FRAME_SAMPLES,
        .raw_channels = 2,
        .processed_channels = 1,
        .mic_channel_index = 0,
        .ref_channel_index = 1,
        .i2c_port = 0,
        .i2c_sda_gpio = 10,
        .i2c_scl_gpio = 7,
        .i2c_speed_hz = 100000,
        .codec_i2c_addr = 0x18,
        .i2c_bus = bus,
        .i2s_port = 0,
        .i2s_dma_desc_num = AUDIO_DMA_DESCRIPTORS,
        .i2s_dma_frame_num = AUDIO_DMA_FRAME_SAMPLES,
        .mclk_multiple = 384,
        .mclk_gpio = 6,
        .bclk_gpio = 5,
        .ws_gpio = 3,
        .dout_gpio = 2,
        .din_gpio = 4,
        .pa_gpio = -1,
        .codec_volume_default = 0xbf,
        .adc_digital_volume = 0xbf,
        .mic_gain_db = 24,
        .max_tracks = 1,
        .track_queue_frames = 2,
        .mic_queue_frames = 2,
        .mic_task_stack_size = 8192,
        .mic_task_priority = 6,
        .mic_task_core_id = 0,
        .speaker_task_stack_size = 4096,
        .speaker_task_priority = 6,
        .speaker_task_core_id = 0,
        .allocator = h2_esp_platform_internal_allocator(),
        .queue_api = h2_esp_platform_queue_api(),
        .sync_api = h2_esp_platform_sync_api(),
        .enable_aec = audio_aec_enabled,
        .defer_aec_init = 1};
    if (rc == 0) rc = h2_esp_es8311_audio_system_init(&audio, &config);
    if (rc == 0) audio_ready = 1;
  }
  if (rc != 0) ESP_LOGE("ai_passport", "audio init failed code=%d", rc);
  h2_pal_audio_t* api =
      rc == 0 ? h2_esp_es8311_audio_system_audio(&audio) : NULL;
  (void)xSemaphoreGive(mutex);
  return api;
}

static int release_audio(void) {
  if (!audio_ready) return 0;
  if (audio.mic_started || audio.playback_started || audio.mic_task != NULL ||
      audio.playback_task != NULL || audio.mic_task_started ||
      audio.playback_task_started)
    return H2_PAL_ERR_BUSY;
  int rc = h2_esp_es8311_audio_system_power_down(&audio);
  if (rc == 0) audio_ready = 0;
  return rc;
}

int h2_esp_board_audio_release(void) {
  int rc = lock();
  if (rc != 0) return rc;
  rc = release_audio();
  (void)xSemaphoreGive(mutex);
  return rc;
}

int h2_esp_board_audio_set_aec_enabled(int enabled) {
  if (enabled != 0 && enabled != 1) return H2_PAL_ERR_INVALID_ARG;
  int rc = lock();
  if (rc != 0) return rc;
  if (enabled != audio_aec_enabled) {
    rc = release_audio();
    if (rc == 0) {
      audio_aec_enabled = enabled;
      ESP_LOGI("ai_passport", "capture mode aec=%d", enabled);
    }
  }
  (void)xSemaphoreGive(mutex);
  return rc;
}

int h2_passport_peripherals_init(void) {
  if (mutex != NULL) return 0;
  mutex = xSemaphoreCreateMutex();
  if (mutex == NULL) return H2_PAL_ERR_NO_MEMORY;
  h2_esp_adc_radio_button_init(&keys);
  int rc = h2_esp_lazy_audio_init(&lazy_audio, NULL, resolve_audio);
  if (rc != 0) {
    vSemaphoreDelete(mutex);
    mutex = NULL;
  }
  return rc;
}

int h2_passport_peripherals_deinit(void) {
  if (mutex == NULL) return 0;
  int rc = lock();
  if (rc != 0) return rc;
  if (audio_ready) {
    rc = h2_esp_es8311_audio_system_deinit(&audio);
    if (rc == 0) audio_ready = 0;
  }
  if (rc == 0 && gauge != NULL) {
    if (i2c_master_bus_rm_device(gauge) == ESP_OK)
      gauge = NULL;
    else
      rc = H2_PAL_ERR_IO;
  }
  if (rc == 0 && bus != NULL) {
    if (i2c_del_master_bus(bus) == ESP_OK)
      bus = NULL;
    else
      rc = H2_PAL_ERR_IO;
  }
  if (rc == 0 && h2_esp_adc_oneshot_deinit(&adc) != ESP_OK) rc = H2_PAL_ERR_IO;
  (void)xSemaphoreGive(mutex);
  if (rc == 0) {
    vSemaphoreDelete(mutex);
    mutex = NULL;
    memset(&lazy_audio, 0, sizeof(lazy_audio));
    audio_aec_enabled = 1;
  }
  return rc;
}

h2_pal_audio_t* h2_passport_audio(void) {
  return h2_esp_lazy_audio_api(&lazy_audio);
}

static int read_keys(void* user, h2_pal_periph_id_t id,
                     h2_pal_radio_button_group_reading_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_pal_radio_button_group_reading_t){.id = id};
  if (id != KEYS) return H2_PAL_ERR_NOT_FOUND;
  int rc = lock();
  if (rc != 0) return rc;
  if (adc == NULL) {
    const h2_esp_adc_oneshot_channel_config_t channel = {
        .channel = ADC_CHANNEL_0,
        .oneshot = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT},
        .enable_calibration = true};
    const h2_esp_adc_oneshot_config_t config = {.unit = {.unit_id = ADC_UNIT_1},
                                                .channels = &channel,
                                                .channel_count = 1};
    if (h2_esp_adc_oneshot_init(&config, &adc) != ESP_OK) rc = H2_PAL_ERR_IO;
  }
  int mv = 0;
  if (rc == 0 && h2_esp_adc_oneshot_read_mv(adc, ADC_CHANNEL_0, &mv) != ESP_OK)
    rc = H2_PAL_ERR_IO;
  if (rc == 0) {
    uint32_t selected = mv < 150 ? UP : mv < 447 ? DOWN : mv < 1900 ? OK : 0;
    if (!h2_esp_adc_radio_button_update(
            &keys,
            selected ? H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED
                     : H2_ESP_ADC_RADIO_BUTTON_SAMPLE_RELEASED,
            selected, &out->pressed_button_id))
      rc = H2_PAL_ERR_INVALID_ARG;
  }
  (void)xSemaphoreGive(mutex);
  return rc;
}

static int read_button(void* user, h2_pal_periph_id_t id,
                       h2_pal_single_button_reading_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_pal_single_button_reading_t){.id = id};
  if (id != UP && id != DOWN && id != OK) return H2_PAL_ERR_NOT_FOUND;
  h2_pal_radio_button_group_reading_t group = {0};
  int rc = read_keys(user, KEYS, &group);
  if (rc == 0)
    out->state = group.pressed_button_id == id ? H2_PAL_BUTTON_STATE_PRESSED
                                               : H2_PAL_BUTTON_STATE_RELEASED;
  return rc;
}

static int read_registers(void* user, uint8_t reg, uint8_t* out, size_t size) {
  return i2c_master_transmit_receive(user, &reg, 1, out, size, 100) == ESP_OK
             ? 0
             : H2_PAL_ERR_IO;
}

static int read_battery(void* user, h2_pal_periph_id_t id,
                        h2_pal_battery_reading_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_pal_battery_reading_t){.id = id};
  if (id != BATTERY) return H2_PAL_ERR_NOT_FOUND;
  int rc = lock();
  if (rc != 0) return rc;
  rc = init_bus();
  if (rc == 0 && gauge == NULL) {
    const i2c_device_config_t config = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                        .device_address = 0x63,
                                        .scl_speed_hz = 100000};
    if (i2c_master_bus_add_device(bus, &config, &gauge) != ESP_OK)
      rc = H2_PAL_ERR_IO;
  }
  const h2_cw2017_transport_t transport = {.user = gauge,
                                           .read_regs = read_registers};
  h2_cw2017_sample_t sample = {0};
  if (rc == 0) rc = h2_cw2017_read_sample(&transport, &sample);
  if (rc == 0) {
    out->voltage_mv = (int32_t)(sample.voltage_uv / 1000u);
    out->percent_x100 = (uint16_t)((uint32_t)sample.soc_q8 * 100u / 256u);
    out->flags =
        H2_PAL_BATTERY_HAS_VOLTAGE_MV | H2_PAL_BATTERY_HAS_PERCENT_X100;
    if (sample.voltage_uv > 0) out->flags |= H2_PAL_BATTERY_PRESENT;
  }
  (void)xSemaphoreGive(mutex);
  return rc;
}

static const h2_pal_periph_radio_button_payload_t key_payload = {.group_id =
                                                                     KEYS};
static const h2_pal_periph_info_t peripherals[] = {
    {.id = UP,
     .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
     .name = "up",
     .payload = &key_payload,
     .payload_size = sizeof(key_payload)},
    {.id = DOWN,
     .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
     .name = "down",
     .payload = &key_payload,
     .payload_size = sizeof(key_payload)},
    {.id = OK,
     .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
     .name = "ok",
     .payload = &key_payload,
     .payload_size = sizeof(key_payload)},
    {.id = KEYS, .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON_GROUP, .name = "keys"},
    {.id = BATTERY, .type = H2_PAL_PERIPH_TYPE_BATTERY, .name = "battery"},
};

static int periph_list(void* user, h2_pal_periph_type_t filter,
                       h2_pal_periph_cb_t callback, void* context) {
  (void)user;
  if (callback == NULL) return H2_PAL_ERR_INVALID_ARG;
  for (unsigned i = 0; i < sizeof(peripherals) / sizeof(peripherals[0]); ++i) {
    if (filter != H2_PAL_PERIPH_TYPE_ANY && filter != peripherals[i].type)
      continue;
    int rc = callback(context, &peripherals[i]);
    if (rc != 0) return rc;
  }
  return 0;
}

static int periph_get(void* user, h2_pal_periph_id_t id,
                      h2_pal_periph_info_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  for (unsigned i = 0; i < sizeof(peripherals) / sizeof(peripherals[0]); ++i)
    if (peripherals[i].id == id) {
      *out = peripherals[i];
      return 0;
    }
  return H2_PAL_ERR_NOT_FOUND;
}

const h2_pal_periph_api_t* h2_passport_periph(void) {
  static const h2_pal_periph_vtable_t vtable = {.list = periph_list,
                                                .get = periph_get};
  static const h2_pal_periph_api_t api = {.vtable = &vtable};
  return &api;
}
const h2_pal_button_api_t* h2_passport_button(void) {
  static const h2_pal_button_vtable_t vtable = {
      .read_single_button = read_button, .read_radio_button_group = read_keys};
  static const h2_pal_button_api_t api = {.vtable = &vtable};
  return &api;
}
const h2_pal_input_api_t* h2_passport_input(void) {
  static const h2_pal_input_vtable_t vtable = {.read_battery = read_battery};
  static const h2_pal_input_api_t api = {.vtable = &vtable};
  return &api;
}
