#include "h2_h2loader_cli_target.h"

#include <assert.h>

#if !defined(_WIN32)
static int handle_event(void *user, const h2_pal_system_event_t *event) {
    (void)user;
    (void)event;
    return H2_PAL_OK;
}
#endif

int main(void) {
    assert(h2_h2loader_cli_target_start() == H2_PAL_OK);
#if !defined(_WIN32)
    h2_pal_system_event_subscription_t *subscription = NULL;
    const h2_pal_system_event_api_t *system_event =
        h2_h2loader_cli_target_system_event();
    assert(h2_pal_system_event_subscribe(
               system_event, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
               handle_event, NULL, &subscription) == H2_PAL_OK);
    assert(subscription != NULL);
    h2_pal_system_event_unsubscribe(system_event, subscription);
#else
    /* Windows exposes serial only, so route events are not a CLI prerequisite. */
    assert(h2_h2loader_cli_target_ble(h2_h2loader_cli_target_mem()) == NULL);
#endif
    h2_h2loader_cli_target_stop();
    return 0;
}
