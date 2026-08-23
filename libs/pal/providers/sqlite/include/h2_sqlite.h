#ifndef H2_SQLITE_H
#define H2_SQLITE_H

#include "h2/pal/os/h2_pal_pref.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_sqlite h2_sqlite_t;

typedef struct h2_sqlite_config {
  /** Database path copied during creation. */
  const char *path;
} h2_sqlite_config_t;

int h2_sqlite_create(const h2_sqlite_config_t *config,
                     h2_sqlite_t **out_provider);
void h2_sqlite_destroy(h2_sqlite_t *provider);
const h2_pal_pref_api_t *h2_sqlite_pref_api(h2_sqlite_t *provider);

#ifdef __cplusplus
}
#endif

#endif
