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
  return 0;
}
