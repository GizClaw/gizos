#ifndef H2_BK_FLASHDB_FAL_CFG_H
#define H2_BK_FLASHDB_FAL_CFG_H

#define FLASHDB_DEV_NAME "flashdb0"
#define H2_BK_PREF_FLASHDB_PATH "h2_pref"

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
    }

#endif
