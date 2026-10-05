#ifndef H2_BK_FLASHDB_FAL_CFG_H
#define H2_BK_FLASHDB_FAL_CFG_H

#define FLASHDB_DEV_NAME "flashdb0"
#define H2_BK_PREF_FLASHDB_PATH "h2_pref"

/* Keep the installed Loader's 24 KiB / 4 KiB KV geometry unchanged.
 * The existing 128 KiB physical FlashDB partition owns the remaining tail. */
#define H2_BK_PREF_LARGE_FLASHDB_PATH "h2_pref_large"
#define H2_BK_PREF_LARGE_OFFSET (CONFIG_FLASHDB_KVDB_START_ADDR + 0x6000u)
#define H2_BK_PREF_LARGE_SIZE 0x1a000u
#define H2_BK_PREF_LARGE_SECTOR_SIZE 0x1000u
#define H2_BK_PREF_INLINE_MAX 3968u

int h2_bk_pref_large_is_blank(void);
int h2_bk_pref_large_empty_headers(void);
#define H2_BK_PREF_LARGE_OWNER_KEY "$h2_large_backing_v1"
int h2_bk_pref_flash_faulted(void);
void h2_bk_pref_flash_clear_fault(void);
void h2_bk_pref_flash_read_begin(void);
void h2_bk_pref_flash_read_end(void);

#define FAL_PART_HAS_TABLE_CFG

extern struct fal_flash_dev g_flashdb0;

#define FAL_FLASH_DEV_TABLE \
    {                       \
        &g_flashdb0,        \
    }

#define FAL_PART_TABLE                                                \
    {                                                                 \
        {                                                             \
            FAL_PART_MAGIC_WORD,                                      \
            H2_BK_PREF_FLASHDB_PATH,                                  \
            FLASHDB_DEV_NAME,                                         \
            CONFIG_FLASHDB_KVDB_START_ADDR,                           \
            CONFIG_FLASHDB_KVDB_SIZE,                                 \
            0u,                                                       \
        },                                                            \
        {                                                             \
            FAL_PART_MAGIC_WORD,                                      \
            H2_BK_PREF_LARGE_FLASHDB_PATH,                            \
            FLASHDB_DEV_NAME,                                         \
            H2_BK_PREF_LARGE_OFFSET,                                  \
            H2_BK_PREF_LARGE_SIZE,                                    \
            0u,                                                       \
        },                                                            \
    }

#endif
