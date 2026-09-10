/* Bench experiment: fixed-address AP XIP, not the production App boot path.
 * No Flash write or BK OTA flag update is performed by this hook. The host
 * installs the separately linked payload only after backing up the device.
 */
#include "modules/ota.h"
#include "soc/soc.h"
#include "armstar.h"

static void h2_bk_fixed_xip_log(const char *text) {
    volatile uint32_t *status = (volatile uint32_t *)0x44820018u;
    volatile uint32_t *fifo = (volatile uint32_t *)0x4482001cu;
    while (*text) {
        while (*status & (1u << 16)) {}
        *fifo = (uint8_t)*text++;
    }
}

static void h2_bk_fixed_xip_probe(void) {
    /* 3808 KiB physical / 34 * 32 = 0x380000 CPU-visible offset.
     * This is the existing s_app start; its stock contents are backed up.
     */
    const uint32_t base = SOC_FLASH_DATA_BASE + 0x00380000u;
    const volatile uint32_t *header = (const volatile uint32_t *)base;
    uint32_t result[4] = {0};
    uint32_t before = bk_ota_get_current_partition();
    uint32_t entry;
    uint32_t irq;
    char line[200];
    typedef void (*probe_fn)(uint32_t *);

    snprintf(line, sizeof(line), "H2_FIXED_XIP loader_partition=%lu base=%08lx magic=%08lx\r\n",
              (unsigned long)before, (unsigned long)base,
              (unsigned long)header[0]);
    h2_bk_fixed_xip_log(line);
    if (before != 0u || header[0] != 0x58324648u ||
        header[2] != base || header[3] != 1u) return;
    entry = header[1];
    if (!(entry & 1u) || entry < base + 0x100u || entry >= base + 0x10000u) {
        snprintf(line, sizeof(line), "H2_FIXED_XIP invalid_entry=%08lx\r\n", (unsigned long)entry);
        h2_bk_fixed_xip_log(line);
        return;
    }
    snprintf(line, sizeof(line), "H2_FIXED_XIP jump=%08lx no_ota_switch=1\r\n", (unsigned long)entry);
    h2_bk_fixed_xip_log(line);
    rtos_delay_milliseconds(100);
    irq = __get_PRIMASK();
    __disable_irq();
    SCB_InvalidateICache();
    __DSB();
    __ISB();
    ((probe_fn)(uintptr_t)entry)(result);
    __set_PRIMASK(irq);
    snprintf(line, sizeof(line), "H2_FIXED_XIP returned pc=%08lx rodata=%08lx value=%08lx iterations=%lu partition=%u\r\n",
              (unsigned long)result[0], (unsigned long)result[1],
              (unsigned long)result[2], (unsigned long)result[3],
              (unsigned)bk_ota_get_current_partition());
    h2_bk_fixed_xip_log(line);
}
