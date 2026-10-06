#ifndef H2_PREF_NOR_OS_H
#define H2_PREF_NOR_OS_H
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#define CONFIG_FLASHDB_USING_KVDB 1
#define CONFIG_FLASHDB_USING_TSDB 0
#define CONFIG_FLASHDB_KVDB_START_ADDR 0x780000u
#define CONFIG_FLASHDB_KVDB_SIZE 0x6000u
#define kNoErr 0
#define BK_LOGI(tag, ...) ((void)(tag))
#define BK_LOGW(tag, ...) ((void)(tag))
#define BK_LOGE(tag, ...) ((void)(tag))
#define BK_LOGD(tag, ...) ((void)(tag))
#ifdef H2_PREF_NOR_DEBUG
#define FDB_PRINT(...) do { if (getenv("H2_PREF_NOR_TRACE")) fprintf(stderr,__VA_ARGS__); } while (0)
#undef BK_LOGI
#define BK_LOGI(tag, ...) FDB_PRINT(__VA_ARGS__)
#else
#define FDB_PRINT(...) ((void)0)
#endif
typedef void *beken_mutex_t;
int rtos_init_mutex(beken_mutex_t *);
int rtos_deinit_mutex(beken_mutex_t *);
int rtos_lock_mutex(beken_mutex_t *);
int rtos_unlock_mutex(beken_mutex_t *);
void *os_malloc(size_t);
void *os_zalloc(size_t);
void *os_realloc(void *,size_t);
void os_free(void *);
#endif
