#ifndef H2_BK_FLASH_STRESS_LAUNCHER_H
#define H2_BK_FLASH_STRESS_LAUNCHER_H

#include "h2/pal/os/h2_pal_disk.h"

enum { H2_BK_FLASH_STRESS_PARTITION = 1 };

/* This adapter exposes only the named diagnostic scratch partition. */
const h2_pal_disk_api_t* h2_bk_flash_stress_disk(void);
/* Minimal management Runtime; no product input/display/audio workers. */
int h2_bk_flash_stress_management_start(void);

#endif
