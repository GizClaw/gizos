#ifndef H2_TEST_PREF_MEM_H
#define H2_TEST_PREF_MEM_H
#include <stddef.h>
void *os_malloc(size_t size);
void *os_zalloc(size_t size);
void *os_realloc(void *memory, size_t size);
void os_free(void *memory);
#endif
