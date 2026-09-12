#ifndef JIELI_WARM_BOOT_H
#define JIELI_WARM_BOOT_H

#include <stdint.h>

/* Flash address of each executable bank's first code byte, as uboot reports
 * it in the boot hand-off (sfc_base_addr). The 32-byte encrypted JieLi file
 * head (BootInfo) sits immediately before it. */
#define H2_JIELI_BANK_1_SFC_BASE UINT32_C(0x00004020)
#define H2_JIELI_BANK_2_SFC_BASE UINT32_C(0x0037c020)

/* Ask the next boot of this Loader to hand control to the bank at sfc_base
 * without BootInfo selecting it. The request is consumed exactly once. */
void h2_jieli_warm_boot_request(uint32_t sfc_base);

/* Number of warm hand-offs performed since RAM lost power, for diagnostics. */
uint32_t h2_jieli_warm_boot_count(void);
uint32_t h2_jieli_warm_boot_last_base(void);

/* SPIKE: sample the flash window under candidate BASE_ADR values next boot. */
void h2_jieli_warm_boot_probe_request(void);

/* Print, once, what the image entered by the last hand-off left behind. */
void h2_jieli_warm_boot_report(void (*write)(const char *line));

#endif
