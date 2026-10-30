#ifndef H2_BK_FLASH_STRESS_H
#define H2_BK_FLASH_STRESS_H

/* Runs the native Flash diagnostic on the caller's controller task. */
void h2_bk_flash_stress_run(void);
/* Minimal management Runtime; no product input/display/audio workers. */
int h2_bk_flash_stress_management_start(void);

#endif
