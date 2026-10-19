#ifndef H2_TEST_PREF_OS_H
#define H2_TEST_PREF_OS_H
#define kNoErr 0
typedef void *beken_mutex_t;
int rtos_init_mutex(beken_mutex_t *mutex);
int rtos_deinit_mutex(beken_mutex_t *mutex);
int rtos_lock_mutex(beken_mutex_t *mutex);
int rtos_unlock_mutex(beken_mutex_t *mutex);
/* Diagnostic sink override for the provider regression, not an SDK API. */
int h2_test_pref_printf(const char *format, ...);
#endif
