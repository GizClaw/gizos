#ifndef H2_MOSAICO_E2E_SCREEN_H
#define H2_MOSAICO_E2E_SCREEN_H
#include "h2/pal/hal/h2_pal_display.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Artifact-owned synchronous presentation. Call before suite resource
 * baselines; no worker/task or shared PAL policy. Display tests suspend it
 * while owning pixels. */
int h2_mosaico_e2e_screen_begin(const h2_pal_display_api_t *display,
                                const char *suite, unsigned position);
int h2_mosaico_e2e_screen_phase(const char *phase);
int h2_mosaico_e2e_screen_result(int test_rc, int confirm_rc);
int h2_mosaico_e2e_screen_release(void);
#ifdef __cplusplus
}
#endif
#endif
