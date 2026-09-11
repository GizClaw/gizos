#ifndef H2_BK_FIXED_FLASH_LAYOUT_CHECK_H
#define H2_BK_FIXED_FLASH_LAYOUT_CHECK_H
/* Compile-time check of the board's fixed-XIP partition table. Include after
 * the SDK partition header (driver/flash_partition.h), which defines the
 * CONFIG_<NAME>_PARTITION_OFFSET/SIZE macros generated from the table. A table
 * without h2_boot_request is not a fixed layout and is not checked. */
#ifdef CONFIG_H2_BOOT_REQUEST_PARTITION_OFFSET

#define H2_FIXED_CHECK_ALIGN (68 * 1024)
#define H2_FIXED_CHECK_OWN_OFFSET CONFIG_APPLICATION_PARTITION_OFFSET
#define H2_FIXED_CHECK_OWN_SIZE \
    (CONFIG_APPLICATION_PARTITION_SIZE + CONFIG_APPLICATION1_PARTITION_SIZE)
#define H2_FIXED_CHECK_OTHER_OFFSET CONFIG_S_APP_PARTITION_OFFSET
#define H2_FIXED_CHECK_OTHER_SIZE CONFIG_S_APP_PARTITION_SIZE

#if CONFIG_APPLICATION_PARTITION_OFFSET + CONFIG_APPLICATION_PARTITION_SIZE != \
    CONFIG_APPLICATION1_PARTITION_OFFSET
#error "fixed-XIP layout: primary_cp_app and primary_ap_app must be contiguous"
#endif

#if (H2_FIXED_CHECK_OWN_OFFSET % H2_FIXED_CHECK_ALIGN) != 0 || \
    (H2_FIXED_CHECK_OWN_SIZE % H2_FIXED_CHECK_ALIGN) != 0 || \
    (H2_FIXED_CHECK_OTHER_OFFSET % H2_FIXED_CHECK_ALIGN) != 0 || \
    (H2_FIXED_CHECK_OTHER_SIZE % H2_FIXED_CHECK_ALIGN) != 0
#error "fixed-XIP layout: Loader and App windows must start and end on 68 KiB boundaries"
#endif

/* The window that starts later is the App window. Loader self-update stages
 * the Loader image in it, so it must hold at least the Loader window. */
#if H2_FIXED_CHECK_OTHER_OFFSET > H2_FIXED_CHECK_OWN_OFFSET
#if H2_FIXED_CHECK_OTHER_SIZE < H2_FIXED_CHECK_OWN_SIZE
#error "fixed-XIP layout: the App window (s_app) must not be smaller than the Loader window"
#endif
#if H2_FIXED_CHECK_OWN_OFFSET + H2_FIXED_CHECK_OWN_SIZE > H2_FIXED_CHECK_OTHER_OFFSET
#error "fixed-XIP layout: the Loader and App windows overlap"
#endif
#else
#if H2_FIXED_CHECK_OWN_SIZE < H2_FIXED_CHECK_OTHER_SIZE
#error "fixed-XIP layout: the App window must not be smaller than the Loader window (s_app)"
#endif
#if H2_FIXED_CHECK_OTHER_OFFSET + H2_FIXED_CHECK_OTHER_SIZE > H2_FIXED_CHECK_OWN_OFFSET
#error "fixed-XIP layout: the Loader and App windows overlap"
#endif
#endif

#if (CONFIG_H2_BOOT_REQUEST_PARTITION_OFFSET % 4096) != 0 || \
    CONFIG_H2_BOOT_REQUEST_PARTITION_SIZE < 4096
#error "fixed-XIP layout: h2_boot_request must be at least one 4 KiB sector, sector aligned"
#endif

#endif
#endif
