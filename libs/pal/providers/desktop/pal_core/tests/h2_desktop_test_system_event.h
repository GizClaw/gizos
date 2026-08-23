#ifndef H2_DESKTOP_TEST_SYSTEM_EVENT_H
#define H2_DESKTOP_TEST_SYSTEM_EVENT_H

#include "h2/pal/os/h2_pal_system_event.h"

int h2_desktop_test_system_event_init(void);
const h2_pal_system_event_api_t *h2_desktop_test_system_event_api(void);
void h2_desktop_test_system_event_deinit(void);

#endif
