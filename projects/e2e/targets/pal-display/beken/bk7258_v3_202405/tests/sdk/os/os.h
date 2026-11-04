#ifndef H2_DISPLAY_LAUNCHER_TEST_OS_H
#define H2_DISPLAY_LAUNCHER_TEST_OS_H
#include <stdint.h>
typedef void* beken_thread_t;
#define kNoErr 0
void rtos_delay_milliseconds(uint32_t milliseconds);
int rtos_core0_create_psram_thread(beken_thread_t* thread, unsigned priority,
                                   const char* name, void (*entry)(void*),
                                   unsigned stack_size, void* user);
#endif
