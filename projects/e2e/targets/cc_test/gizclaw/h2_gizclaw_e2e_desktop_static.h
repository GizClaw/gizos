#ifndef H2_GIZCLAW_E2E_DESKTOP_STATIC_H
#define H2_GIZCLAW_E2E_DESKTOP_STATIC_H

#include "h2_atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Borrowed process-lifetime values with independent C11 file-static backing. */
h2_atomic_flag_t *h2_gizclaw_e2e_desktop_running_flag(void);
h2_atomic_bool_t *h2_gizclaw_e2e_desktop_stop_requested(void);

#ifdef __cplusplus
}
#endif

#endif
