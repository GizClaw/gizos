#ifndef H2_JIELI_AC791N_DEVKIT_FLASH_WINDOW_H
#define H2_JIELI_AC791N_DEVKIT_FLASH_WINDOW_H

/* Task-context, board-wide NOR protection lease. Zero-initialize each token;
 * do not copy or share a live token. Close every successful open, including
 * failed/short operations. Nested/overlapping leases retain the first saved
 * SDK protection value until the last close; no lock spans caller operations.
 * Uses GET/SET_WRITE_PROTECT because SDK suspend/resume has one backup slot,
 * not a nesting count. A failed restore poisons future opens until reboot.
 * This restores the SDK's configured value, not independently read status bits.
 */
typedef struct { int active; } h2_jieli_flash_window_t;
int h2_jieli_flash_window_open(h2_jieli_flash_window_t *window);
int h2_jieli_flash_window_close(h2_jieli_flash_window_t *window);

#endif
