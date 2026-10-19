#ifndef H2_TEST_EASYFLASH_H
#define H2_TEST_EASYFLASH_H
#include <stddef.h>
#define EF_NO_ERR 0
int easyflash_init(void);
size_t ef_get_env_blob(const char *key, void *data, size_t size, size_t *saved);
int ef_del_env(const char *key);
#endif
