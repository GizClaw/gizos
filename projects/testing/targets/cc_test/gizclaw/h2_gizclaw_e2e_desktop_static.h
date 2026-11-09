#ifndef H2_GIZCLAW_E2E_DESKTOP_STATIC_H
#define H2_GIZCLAW_E2E_DESKTOP_STATIC_H

#include "h2_atomic.h"

/* Borrowed process-lifetime values with independent C11 file-static backing. */
H2_ATOMIC_DECLARE_STATIC(flag, h2_gizclaw_e2e_desktop_running_flag);
H2_ATOMIC_DECLARE_STATIC(bool, h2_gizclaw_e2e_desktop_stop_requested);

#endif
