#ifndef BUFFET_DESKTOP_PREF_H
#define BUFFET_DESKTOP_PREF_H
#include "h2_sqlite.h"

/* Own a persistent Pref backend until Runtime and all namespaces are closed.
 * NULL root selects H2_DESKTOP_STORAGE_ROOT, then the native user data folder.
 * Explicit roots must be absolute. Initializes an empty database so the App's
 * first read returns NOT_FOUND rather than failing to open a missing file. */
int buffet_desktop_pref_create(const char* root, h2_sqlite_t** out);
#endif
