#include "h2_runtime.h"

/*
 * This translation unit is intentionally separate from reader polling tests.
 * Compiling it proves that the app-facing Runtime surface exposes the parallel
 * card-emulation contract without changing or aliasing h2_pal_nfc_api_t.
 */
_Static_assert(
    sizeof(((h2_runtime_t *)0)->nfc_card_emulation) ==
        sizeof(const h2_pal_nfc_card_emulation_api_t *),
    "Runtime must expose the card-emulation API object directly");

void h2_runtime_nfc_card_emulation_contract_compile_test(void) {
    h2_runtime_t runtime = {0};
    runtime.nfc_card_emulation = NULL;
    runtime.nfc = NULL;
    (void)runtime;
}
