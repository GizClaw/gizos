#ifndef H2_PREF_NOR_EF_H
#define H2_PREF_NOR_EF_H
#include <stddef.h>
#define EF_NO_ERR 0
int easyflash_init(void);
size_t ef_get_env_blob(const char *,void *,size_t,size_t *);
int ef_del_env(const char *);
#endif
