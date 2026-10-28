#ifndef H2_BK_TASK_TRACE_TEST_SDK_H
#define H2_BK_TASK_TRACE_TEST_SDK_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t __get_IPSR(void);
uint32_t __get_PRIMASK(void);
uint32_t __get_BASEPRI(void);
void* psram_malloc(size_t size);
void os_free(void* memory);
int h2_bk_trace_test_output(const char* format, ...);
#ifdef __cplusplus
}
#endif
#define BK_DUMP_OUT(...) h2_bk_trace_test_output(__VA_ARGS__)
#endif
