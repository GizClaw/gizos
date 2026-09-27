#include "h2_gizclaw_e2e_desktop_static.h"

#include "h2_atomic_static.h"

H2_ATOMIC_DEFINE_STATIC_ACCESSOR(flag, h2_gizclaw_e2e_desktop_running_flag, 0u);
H2_ATOMIC_DEFINE_STATIC_ACCESSOR(bool, h2_gizclaw_e2e_desktop_stop_requested, false);
