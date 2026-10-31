#ifndef H2_BK_PAL_DISK_LAUNCHER_H
#define H2_BK_PAL_DISK_LAUNCHER_H

#include "h2/pal/os/h2_pal_disk.h"

enum { H2_BK_PAL_DISK_PARTITION = 1 };

/* This adapter exposes only the named diagnostic scratch partition. */
const h2_pal_disk_api_t* h2_bk_pal_disk_disk(void);
/* Minimal management Runtime; no product input/display/audio workers. */
int h2_bk_pal_disk_management_start(void);

#endif
