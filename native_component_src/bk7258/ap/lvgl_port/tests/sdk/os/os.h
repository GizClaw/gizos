#ifndef H2_BK_DMA2D_TEST_OS_H
#define H2_BK_DMA2D_TEST_OS_H
#include <common/bk_include.h>
typedef struct test_semaphore *beken_semaphore_t;
int rtos_init_semaphore_ex(beken_semaphore_t *out, unsigned maximum, unsigned initial);
int rtos_deinit_semaphore(beken_semaphore_t *semaphore);
int rtos_set_semaphore(beken_semaphore_t *semaphore);
int rtos_get_semaphore(beken_semaphore_t *semaphore, unsigned timeout_ms);
#endif
