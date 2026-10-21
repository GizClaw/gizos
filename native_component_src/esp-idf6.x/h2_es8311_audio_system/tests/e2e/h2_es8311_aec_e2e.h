#ifndef H2_ES8311_AEC_E2E_H
#define H2_ES8311_AEC_E2E_H
#include "h2_esp_es8311_audio_system.h"
/* Manual, single-owner native diagnostics; synchronous, prints bounded metrics.
 * Algorithm probe uses synthetic PCM; live probe borrows product config and
 * exclusively owns its codec/I2S during the call. Neither qualifies acoustics.
 * C3 diagnostics require 160 MHz. Run on an internal stack >=16 KiB.
 */
/** @return Number of SDK modes that executed (six means all modes). */
int h2_es8311_aec_modes_e2e(void);
/** @return Failed synthetic signal, average budget and cleanup checks. */
int h2_es8311_aec_e2e(void);
/**
 * @brief Exercise a borrowed C3 config synchronously with exclusive hardware access.
 * @param config Borrowed for the call; 16 kHz, 320-sample PAL frames, two raw
 * lanes with distinct mic/reference indices, mono output and AEC enabled.
 * @return Number of failed capture, lifecycle and resource checks.
 */
int h2_es8311_aec_live_e2e(const h2_esp_es8311_audio_system_config_t *config);
#endif
