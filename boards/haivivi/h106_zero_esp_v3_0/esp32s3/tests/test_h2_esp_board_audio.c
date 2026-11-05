/* Links the production board audio source with SDK/provider doubles.
 * Every post-sleep call uses the original API value retained by Runtime. */
#include "h2_esp_board_private.h"
#include "h2_esp_board_config.h"
#include "h2_esp_zero_esp_board_internal.h"
#include "h2_esp_es8311_audio_system.h"
#include "h2_esp_lazy_audio.h"
#include "driver/gpio.h"
#include <assert.h>
#include <string.h>

static unsigned s_init, s_down, s_deinit, s_power, s_lazy_init;
static int s_down_result, s_power_result, s_deinit_result;
static int backend_info(void* user, h2_audio_info_t* info) {
  assert(user != NULL);
  memset(info, 0, sizeof(*info));
  info->available = 1;
  info->playback_supported = 1;
  return H2_AUDIO_OK;
}
static int backend_speaker(void* user) {
  assert(user != NULL);
  return H2_AUDIO_OK;
}
static const h2_pal_audio_vtable_t s_backend = {
    .get_info = backend_info,
    .start_speaker = backend_speaker,
};
static h2_pal_audio_t* resolve(h2_esp_lazy_audio_t* lazy) {
  /* Fail deterministically at the cleared callback behind the real crash. */
  assert(lazy->resolve != NULL);
  return lazy->resolve(lazy->user);
}
static int lazy_info(void* user, h2_audio_info_t* info) {
  h2_pal_audio_t* api = resolve(user);
  return api == NULL ? H2_AUDIO_ERR_UNAVAILABLE
                     : h2_pal_audio_get_info(api, info);
}
static int lazy_speaker(void* user) {
  h2_pal_audio_t* api = resolve(user);
  return api == NULL ? H2_AUDIO_ERR_UNAVAILABLE
                     : h2_pal_audio_start_speaker(api);
}
static const h2_pal_audio_vtable_t s_lazy = {
    .get_info = lazy_info,
    .start_speaker = lazy_speaker,
};
int h2_esp_lazy_audio_init(h2_esp_lazy_audio_t* lazy, void* user,
                           h2_esp_lazy_audio_resolve_fn callback) {
  ++s_lazy_init;
  *lazy = (h2_esp_lazy_audio_t){.user = user, .resolve = callback};
  lazy->api = (h2_pal_audio_t){.user = lazy, .vtable = &s_lazy};
  return H2_AUDIO_OK;
}
h2_pal_audio_t* h2_esp_lazy_audio_api(h2_esp_lazy_audio_t* lazy) {
  return &lazy->api;
}
int h2_esp_es8311_audio_system_init(
    h2_esp_es8311_audio_system_t* system,
    const h2_esp_es8311_audio_system_config_t* config) {
  assert(config->sample_rate_hz == 16000u && config->max_tracks == 4);
  ++s_init;
  system->audio = (h2_pal_audio_t){.user = system, .vtable = &s_backend};
  return H2_AUDIO_OK;
}
int h2_esp_es8311_audio_system_deinit(h2_esp_es8311_audio_system_t* system) {
  assert(system != NULL);
  ++s_deinit;
  return s_deinit_result;
}
int h2_esp_es8311_audio_system_power_down(
    h2_esp_es8311_audio_system_t* system) {
  assert(system != NULL);
  ++s_down;
  return s_down_result;
}
int h2_esp_es8311_audio_system_prepare(h2_esp_es8311_audio_system_t* system) {
  assert(system != NULL);
  return H2_AUDIO_OK;
}
h2_pal_audio_t* h2_esp_es8311_audio_system_audio(
    h2_esp_es8311_audio_system_t* system) {
  return &system->audio;
}
int h2_esp_zero_esp_board_modem_atomics_prepare(void) { return H2_PAL_OK; }
int h2_esp_zero_esp_board_enable_peripheral_power(void) {
  ++s_power;
  return s_power_result;
}
i2c_master_bus_handle_t h2_esp_zero_esp_board_i2c_bus(void) { return NULL; }
int h2_esp_zero_esp_board_codec_pa_set(void* user, int enabled) {
  (void)user;
  (void)enabled;
  return H2_AUDIO_OK;
}
h2_pal_mem_api_t* h2_esp_board_psram_allocator(void) { return NULL; }
const h2_pal_queue_api_t* h2_esp_board_queue_api(void) { return NULL; }
const h2_pal_sync_api_t* h2_esp_board_sync_api(void) { return NULL; }
esp_err_t gpio_config(const gpio_config_t* config) {
  assert(config != NULL);
  return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level) {
  (void)gpio;
  (void)level;
  return ESP_OK;
}

static void available(h2_pal_audio_t* borrowed) {
  h2_audio_info_t info;
  assert(h2_pal_audio_get_info(borrowed, &info) == H2_AUDIO_OK);
  assert(info.available && info.playback_supported);
  assert(h2_pal_audio_start_speaker(borrowed) == H2_AUDIO_OK);
}
int main(void) {
  assert(h2_esp_board_audio_deinit() == H2_PAL_OK);
  h2_pal_audio_t borrowed = *h2_esp_board_audio();
  assert(s_lazy_init == 1u && s_init == 0u);
  /* An unused borrowed interface also survives sleep preparation. */
  assert(h2_esp_board_audio_power_down() == H2_PAL_OK);
  assert(s_down == 0u && s_power == 0u);
  available(&borrowed);
  for (unsigned cycle = 0; cycle < 3u; ++cycle) {
    unsigned initialized = s_init, powered = s_power, down = s_down;
    assert(h2_esp_board_audio_power_down() == H2_PAL_OK);
    assert(h2_esp_board_audio_if_initialized() == NULL);
    assert(h2_esp_board_audio_power_down() == H2_PAL_OK);
    assert(s_down == down + 1u);
    assert(s_init == initialized && s_power == powered && s_lazy_init == 1u);
    /* Failed rail restoration remains retryable via the retained API. */
    s_power_result = H2_PAL_ERR_IO;
    h2_audio_info_t info;
    assert(h2_pal_audio_get_info(&borrowed, &info) == H2_AUDIO_ERR_UNAVAILABLE);
    assert(s_init == initialized);
    s_power_result = H2_PAL_OK;
    available(&borrowed);
    assert(s_init == initialized + 1u);
    assert(h2_esp_board_audio()->user == borrowed.user);
    assert(h2_esp_board_audio()->vtable == borrowed.vtable);
  }
  s_down_result = H2_AUDIO_ERR_IO;
  unsigned initialized = s_init;
  assert(h2_esp_board_audio_power_down() == H2_AUDIO_ERR_IO);
  assert(h2_esp_board_audio_if_initialized() != NULL);
  available(&borrowed);
  assert(s_init == initialized);
  s_down_result = H2_AUDIO_OK;
  s_deinit_result = H2_AUDIO_ERR_IO;
  assert(h2_esp_board_audio_deinit() == H2_AUDIO_ERR_IO);
  available(&borrowed);
  s_deinit_result = H2_AUDIO_OK;
  /* Full teardown may invalidate it; the next Runtime acquires anew. */
  assert(h2_esp_board_audio_deinit() == H2_PAL_OK);
  assert(h2_esp_board_audio_if_initialized() == NULL);
  assert(h2_esp_board_audio_deinit() == H2_PAL_OK);
  borrowed = *h2_esp_board_audio();
  assert(s_lazy_init == 2u && s_deinit == 2u);
  available(&borrowed);
  assert(h2_esp_board_audio_deinit() == H2_PAL_OK);
  return 0;
}
