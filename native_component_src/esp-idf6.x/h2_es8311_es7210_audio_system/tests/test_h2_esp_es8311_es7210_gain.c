#include "h2_esp_es8311_es7210_gain.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
  const uint8_t gains[H2_ESP_ES8311_ES7210_GAIN_INPUT_COUNT] = {
      30u,
      30u,
      12u,
      0u,
  };

  assert(h2_esp_es8311_es7210_input_gain_register(24u, 0u, gains, 0u) == 0x18u);
  assert(h2_esp_es8311_es7210_input_gain_register(24u, 0x07u, gains, 0u) ==
         0x1au);
  assert(h2_esp_es8311_es7210_input_gain_register(24u, 0x07u, gains, 1u) ==
         0x1au);
  assert(h2_esp_es8311_es7210_input_gain_register(24u, 0x07u, gains, 2u) ==
         0x14u);
  /* ES7210 DS rev21, registers 0x43..0x46: upper steps are not 3 dB apart:
   * 0xB=33, 0xC=34.5, 0xD=36, 0xE=37.5 dB. Integer 38 selects the top step. */
  const struct {
    uint32_t requested;
    uint8_t expected;
  } upper[] = {{33, 0x1b}, {34, 0x1b}, {35, 0x1c},
               {36, 0x1d}, {37, 0x1d}, {38, 0x1e}};
  for (unsigned i = 0; i < sizeof(upper) / sizeof(upper[0]); ++i)
    assert(h2_esp_es8311_es7210_input_gain_register(
               upper[i].requested, 0, gains, 0) == upper[i].expected);
  assert(h2_esp_es8311_es7210_zero_legacy_ref_gain(1, 0u, 2u));
  assert(!h2_esp_es8311_es7210_zero_legacy_ref_gain(1, 0x04u, 2u));
  assert(!h2_esp_es8311_es7210_zero_legacy_ref_gain(0, 0u, 2u));
  return 0;
}
