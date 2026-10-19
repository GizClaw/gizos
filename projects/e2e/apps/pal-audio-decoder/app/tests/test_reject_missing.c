#include "h2_pal_audio_decoder_e2e.h"
#include <assert.h>

int main(void) {
    h2_pal_adec_result_t result;
    const h2_pal_audio_decoder_api_t missing = {0};
    const h2_pal_adec_config_t config = {.decoder = &missing};
    assert(h2_pal_audio_decoder_e2e_run(&config, &result) != H2_PAL_OK);
    assert(!result.qualified && result.passed == 0u);
    assert(result.blocked == H2_PAL_ADEC_CASE_COUNT);
    for (size_t i = 0u; i < H2_PAL_ADEC_CASE_COUNT; ++i)
        assert(result.cases[i].id && result.cases[i].status == H2_PAL_ADEC_BLOCKED);
    return 0;
}
