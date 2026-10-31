#ifdef NDEBUG
#undef NDEBUG
#endif
#include "bsp/esp_mosaico.h"
#include "h2_mosaico_audio.h"
#include "h2_chip_board.h"
#include "h2_esp_es8311_audio_system.h"
#include <assert.h>
static int off_error, init_error, deinit_error, alive;
static unsigned initialized_count, destroyed_count;
static const h2_pal_audio_api_t audio = {0};
#ifdef H2_MOSAICO_MFG_DAC_OSR20
static uint8_t dac_clock;
static int prepare_error, read_error, write_error;
int h2_esp_es8311_audio_system_prepare(h2_esp_es8311_audio_system_t* s) {
  s->codec = &alive;
  dac_clock = 0x90;
  return prepare_error;
}
int i2c_master_transmit_receive(void* codec, const uint8_t* reg, size_t tx,
                                uint8_t* value, size_t rx, int timeout) {
  assert(codec == &alive && *reg == 4 && tx == 1 && rx == 1 && timeout == 100);
  *value = dac_clock;
  return read_error;
}
int i2c_master_transmit(void* codec, const uint8_t* data, size_t count,
                        int timeout) {
  assert(codec == &alive && count == 2 && data[0] == 4 && timeout == 100);
  assert(data[1] == 0xa0); /* Preserve bit7, replace only private OSR value. */
  dac_clock = data[1];
  return write_error;
}
#endif
int bsp_board_variant_get(bsp_board_variant_t* out) {
  *out = BSP_BOARD_VARIANT_V1_2;
  return 0;
}
int bsp_power_set_vcc_3v3(bool on) {
  assert(on);
  return 0;
}
int bsp_power_set_codec_3v3(bool on) { return !on ? off_error : 0; }
void* bsp_i2c_get_handle(void) { return &alive; }
h2_pal_mem_api_t* h2_esp_platform_default_allocator(void) { return NULL; }
const h2_pal_queue_api_t* h2_esp_platform_queue_api(void) { return NULL; }
const h2_pal_sync_api_t* h2_esp_platform_sync_api(void) { return NULL; }
int h2_esp_es8311_audio_system_init(
    h2_esp_es8311_audio_system_t* s,
    const h2_esp_es8311_audio_system_config_t* c) {
  assert(s && !alive && !c->enable_aec && c->sample_rate_hz == 16000 &&
         c->i2c_sda_gpio == 56);
  ++initialized_count;
  if (!init_error) alive = 1;
  return init_error;
}
int h2_esp_es8311_audio_system_deinit(h2_esp_es8311_audio_system_t* s) {
  assert(s && alive);
  ++destroyed_count;
  if (!deinit_error) alive = 0;
  return deinit_error;
}
const h2_pal_audio_api_t* h2_esp_es8311_audio_system_audio(
    h2_esp_es8311_audio_system_t* s) {
  assert(s && alive);
  return &audio;
}
int main(void) {
  assert(h2_mosaico_audio_deinit() == 0);
  assert(h2_mosaico_audio_init() == 0 && h2_mosaico_audio() == &audio);
  assert(h2_mosaico_audio_init() == 0 && initialized_count == 1);
  deinit_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_mosaico_audio_deinit() == H2_PAL_ERR_TIMEOUT && alive);
  deinit_error = 0;
  off_error = 1;
  assert(h2_mosaico_audio_deinit() == H2_PAL_ERR_IO && !alive);
  assert(h2_mosaico_audio() == h2_pal_unsupported_audio_api());
  assert(h2_mosaico_audio_init() == H2_PAL_ERR_INVALID_STATE);
  off_error = 0;
  assert(h2_mosaico_audio_deinit() == 0 && destroyed_count == 2);
  assert(h2_mosaico_audio_deinit() == 0 && destroyed_count == 2);
  init_error = H2_AUDIO_ERR_NO_MEMORY;
  off_error = 1;
  assert(h2_mosaico_audio_init() == H2_AUDIO_ERR_NO_MEMORY && !alive);
  assert(h2_mosaico_audio_init() == H2_PAL_ERR_INVALID_STATE);
  off_error = 0;
  assert(h2_mosaico_audio_deinit() == 0 && destroyed_count == 2);
  init_error = 0;
  assert(h2_mosaico_audio_init() == 0);
  assert(h2_mosaico_audio_deinit() == 0 && !alive);
#ifdef H2_MOSAICO_MFG_DAC_OSR20
  prepare_error = H2_AUDIO_ERR_IO;
  assert(h2_mosaico_audio_init() == H2_AUDIO_ERR_IO && !alive);
  prepare_error = 0;
  read_error = 1;
  assert(h2_mosaico_audio_init() == H2_PAL_ERR_IO && !alive);
  read_error = 0;
  write_error = 1;
  assert(h2_mosaico_audio_init() == H2_PAL_ERR_IO && !alive);
  write_error = 0;
  prepare_error = H2_PAL_ERR_IO;
  deinit_error = H2_PAL_ERR_BUSY;
  assert(h2_mosaico_audio_init() == H2_PAL_ERR_IO && alive);
  assert(h2_mosaico_audio() == h2_pal_unsupported_audio_api());
  assert(h2_mosaico_audio_init() == H2_PAL_ERR_INVALID_STATE);
  assert(h2_mosaico_audio_deinit() == H2_PAL_ERR_BUSY && alive);
  deinit_error = prepare_error = 0;
  assert(h2_mosaico_audio_deinit() == 0 && !alive);
  assert(h2_mosaico_audio_init() == 0 && dac_clock == 0xa0);
  assert(h2_mosaico_audio_deinit() == 0 && !alive);
#endif
  return 0;
}
