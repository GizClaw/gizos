#include "h2_gizclaw_e2e_desktop_static.h"

#include "h2_atomic_static.h"

H2_ATOMIC_DEFINE_STATIC(flag, s_running, 0u);
H2_ATOMIC_DEFINE_STATIC(bool, s_stop_requested, false);

h2_atomic_flag_t *h2_gizclaw_e2e_desktop_running_flag(void) {
    return &s_running;
}

h2_atomic_bool_t *h2_gizclaw_e2e_desktop_stop_requested(void) {
    return &s_stop_requested;
}
