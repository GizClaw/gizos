#include "h2_desktop_app_support_c.h"

#include "h2_desktop_app_support.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <new>

struct h2_desktop_network_services {
  h2::desktop::OwnedNetworkServices impl;
};

extern "C" h2_pal_result_t h2_desktop_network_services_create(
    int with_mqtt, int with_webrtc,
    h2_desktop_network_services_t **out_services) {
  if (out_services == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_services = nullptr;
  auto *services = new (std::nothrow) h2_desktop_network_services_t();
  if (services == nullptr) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  const h2_pal_result_t result = static_cast<h2_pal_result_t>(
      h2::desktop::open_network_services(with_mqtt != 0, with_webrtc != 0,
                                         &services->impl));
  if (result != H2_PAL_OK) {
    delete services;
    return result;
  }
  *out_services = services;
  return H2_PAL_OK;
}

extern "C" void h2_desktop_network_services_destroy(
    h2_desktop_network_services_t *services) {
  delete services;
}

extern "C" const h2_pal_net_api_t *h2_desktop_host_net_api(void) {
  return h2::desktop::host_net_api();
}

extern "C" const h2_pal_crypto_api_t *h2_desktop_network_services_crypto(
    const h2_desktop_network_services_t *services) {
  return services == nullptr ? h2_pal_unsupported_crypto_api()
                             : services->impl.crypto();
}

extern "C" const h2_pal_mqtt_api_t *h2_desktop_network_services_mqtt(
    const h2_desktop_network_services_t *services) {
  return services == nullptr ? h2_pal_unsupported_mqtt_api()
                             : services->impl.mqtt();
}

extern "C" const h2_pal_webrtc_api_t *h2_desktop_network_services_webrtc(
    const h2_desktop_network_services_t *services) {
  return services == nullptr ? h2_pal_unsupported_webrtc_api()
                             : services->impl.webrtc();
}
