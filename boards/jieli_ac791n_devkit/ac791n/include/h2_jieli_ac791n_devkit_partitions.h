#ifndef H2_JIELI_AC791N_DEVKIT_PARTITIONS_H
#define H2_JIELI_AC791N_DEVKIT_PARTITIONS_H

#include <stdint.h>

/*
 * Physical 8 MiB NOR layout for the AC791N development board. JieLi's
 * double-bank packer owns [0, 0x700000); Loader and App are logical boot roles,
 * not fixed raw-flash ranges. Only the tail partitions have stable addresses.
 */
#define H2_JIELI_FLASH_SIZE UINT32_C(0x00800000)
#define H2_JIELI_FLASH_SECTOR_SIZE UINT32_C(0x00001000)

#define H2_JIELI_EXECUTABLE_REGION_ADDRESS UINT32_C(0x00000000)
#define H2_JIELI_EXECUTABLE_REGION_SIZE UINT32_C(0x00700000)
/* CODE_BOUNDARY_LINE emitted by the wl82 double-bank packer. */
#define H2_JIELI_IMAGE_MAX_SIZE UINT32_C(0x0037d000)
#define H2_JIELI_APP_ENTRY_PATH "app/jieli/update.ufw"

#define H2_JIELI_PREF_ADDRESS UINT32_C(0x00700000)
#define H2_JIELI_PREF_SIZE UINT32_C(0x00040000)

#define H2_JIELI_COREDUMP_ADDRESS UINT32_C(0x00740000)
#define H2_JIELI_COREDUMP_SIZE UINT32_C(0x00040000)

#define H2_JIELI_VENDOR_ADDRESS UINT32_C(0x00780000)
#define H2_JIELI_VENDOR_SIZE UINT32_C(0x0007f000)

/* JieLi UBOOT requires the final 4 KiB and the packer accounts for it. */
#define H2_JIELI_BOOT_ADDRESS UINT32_C(0x007ff000)
#define H2_JIELI_BOOT_SIZE UINT32_C(0x00001000)

#define H2_JIELI_PARTITION_LOADER UINT32_C(1)
#define H2_JIELI_PARTITION_APP UINT32_C(2)
#define H2_JIELI_PARTITION_PREF UINT32_C(3)
#define H2_JIELI_PARTITION_COREDUMP UINT32_C(4)
#define H2_JIELI_PARTITION_VENDOR UINT32_C(5)
#define H2_JIELI_PARTITION_BOOT UINT32_C(6)

#if H2_JIELI_EXECUTABLE_REGION_ADDRESS + H2_JIELI_EXECUTABLE_REGION_SIZE != H2_JIELI_PREF_ADDRESS
#error "AC791N executable region and pref partition must be contiguous"
#endif
#if H2_JIELI_PREF_ADDRESS + H2_JIELI_PREF_SIZE != H2_JIELI_COREDUMP_ADDRESS
#error "AC791N pref and coredump partitions must be contiguous"
#endif
#if H2_JIELI_COREDUMP_ADDRESS + H2_JIELI_COREDUMP_SIZE != H2_JIELI_VENDOR_ADDRESS
#error "AC791N coredump and vendor partitions must be contiguous"
#endif
#if H2_JIELI_VENDOR_ADDRESS + H2_JIELI_VENDOR_SIZE != H2_JIELI_BOOT_ADDRESS
#error "AC791N vendor and boot partitions must be contiguous"
#endif
#if H2_JIELI_BOOT_ADDRESS + H2_JIELI_BOOT_SIZE != H2_JIELI_FLASH_SIZE
#error "AC791N partitions must cover the complete physical flash"
#endif

#endif
