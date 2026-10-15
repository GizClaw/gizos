#include "h2_pal_audio_e2e.h"
#include <assert.h>

int main(void) {
  h2_pal_audio_api_t missing = {0};
  h2_pal_audio_e2e_config_t config = {.audio = &missing};
  h2_pal_audio_e2e_result_t result;
  assert(h2_pal_audio_e2e_run(&config, &result) != H2_AUDIO_OK);
  assert(result.passed < H2_PAL_AUDIO_E2E_CASE_COUNT);
  assert(result.failed + result.blocked > 0u);
  return 0;
}
