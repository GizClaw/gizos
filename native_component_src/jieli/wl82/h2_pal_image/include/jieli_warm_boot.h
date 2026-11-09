#ifndef JIELI_WARM_BOOT_H
#define JIELI_WARM_BOOT_H

#include "h2_jieli_wl82_boot.h"

/* Number of warm hand-offs performed since RAM lost power, for diagnostics. */
uint32_t h2_jieli_warm_boot_count(void);
uint32_t h2_jieli_warm_boot_last_base(void);
uint32_t h2_jieli_warm_boot_running_base(int native_result,
                                         uint32_t native_base);
/* Consume the ROM-selected source captured before this warm remap. Zero means
 * absent, legacy, torn or already consumed evidence; never infer a source. */
uint32_t h2_jieli_warm_boot_take_rom_base(void);

/* Print, once, what the image entered by the last hand-off left behind. */
void h2_jieli_warm_boot_report(void (*write)(const char* line));

/* Retained-layout helpers used only by the separately linked early consumer. */
void h2_jieli_warm_boot_snapshot_previous(void);
void h2_jieli_warm_boot_mark_handoff(uint32_t sfc_base);

#endif
