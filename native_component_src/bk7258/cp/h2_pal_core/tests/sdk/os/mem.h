#ifndef TEST_OS_MEM_H
#define TEST_OS_MEM_H
#include <stddef.h>
void *os_malloc(size_t size);
void os_free(void *ptr);
void *os_memset(void *ptr, int value, size_t size);
#endif
