#ifndef H2_JIELI_LOADER_PLATFORM_H
#define H2_JIELI_LOADER_PLATFORM_H

#include "h2_loader_package.h"
#include "h2/pal/hal/h2_pal_power.h"
#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_pref.h"

int h2_jieli_loader_platform_init(
    const h2_pal_fs_api_t *fs,
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator);
const h2_pal_power_api_t *h2_jieli_loader_power_api(void);
const h2_loader_image_reader_api_t *h2_jieli_loader_image_reader(void);
const h2_loader_image_writer_api_t *h2_jieli_loader_image_writer(void);
int h2_jieli_loader_confirm_active_image(void *user);

#endif
