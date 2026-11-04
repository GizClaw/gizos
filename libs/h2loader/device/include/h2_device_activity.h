#ifndef H2_DEVICE_ACTIVITY_H
#define H2_DEVICE_ACTIVITY_H

#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One activity observer for the device's management service. The binding and
 * callback context are borrowed until unbind returns. Late native callbacks
 * use a boot-lifetime generation token and never dereference a released
 * binding. Bind/unbind are caller-serialized. */
typedef struct h2_device_activity {
  const h2_pal_wifi_sta_api_t* wifi;
  const h2_pal_time_api_t* time;
  h2_pal_wifi_activity_fn callback;
  void* user;
} h2_device_activity_t;

int h2_device_activity_bind(h2_device_activity_t* binding,
                            const h2_pal_wifi_sta_api_t* wifi,
                            const h2_pal_time_api_t* time,
                            h2_pal_wifi_activity_fn callback, void* user);
void h2_device_activity_unbind(h2_device_activity_t* binding);

#ifdef __cplusplus
}
#endif

#endif
