#include "h2_g99_e2e_internal.h"

/* Run order. Startup first: it owns the boot-time checks. Wi-Fi last: it
 * restarts Main with a connected station. */
const h2_g99_e2e_group_t* const h2_g99_e2e_groups[] = {
    &h2_g99_e2e_startup_group,  &h2_g99_e2e_navigation_group,
    &h2_g99_e2e_keys_group,     &h2_g99_e2e_layout_group,
    &h2_g99_e2e_touch_group,    &h2_g99_e2e_perf_group,
    &h2_g99_e2e_settings_group, &h2_g99_e2e_wifi_group,
};

const size_t h2_g99_e2e_group_count =
    sizeof(h2_g99_e2e_groups) / sizeof(h2_g99_e2e_groups[0]);
