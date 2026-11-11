#include "h2_alipay_sdk_internal.h"
#include "h2_alipay_sdk_socket_abi.h"
#include <stdio.h>
#include <string.h>

void alipay_iot_gettimeofday(alipay_iot_timeval* out) {
  if (out == NULL) return;
  uint64_t ms = 0u;
  if (h2_pal_time_get_wall_ms(h2_alipay_sdk_config()->time, &ms) != H2_PAL_OK)
    ms = 0u;
  out->tv_sec = (long)(ms / 1000u);
  out->tv_usec = (long)(ms % 1000u) * 1000;
}
void alipay_task_sleep(uint32_t ms) {
  (void)h2_pal_time_sleep_ms(h2_alipay_sdk_config()->time, ms);
}
alipay_iot_mutex alipay_iot_mutex_create(const char* name) {
  const h2_alipay_sdk_config_t* config = h2_alipay_sdk_config();
  const h2_pal_mutex_config_t mutex_config = {
      .name = name,
      .allocator = config->mem,
      .flags = H2_PAL_MUTEX_FLAG_NONE,
  };
  h2_pal_mutex_t* mutex = NULL;
  return h2_pal_mutex_create(config->sync, &mutex_config, &mutex) == H2_PAL_OK
             ? mutex
             : NULL;
}
int alipay_iot_mutex_lock(alipay_iot_mutex mutex) {
  return h2_pal_mutex_lock(h2_alipay_sdk_config()->sync, mutex) == H2_PAL_OK
             ? 0
             : -1;
}
int alipay_iot_mutex_unlock(alipay_iot_mutex mutex) {
  return h2_pal_mutex_unlock(h2_alipay_sdk_config()->sync, mutex) == H2_PAL_OK
             ? 0
             : -1;
}
int alipay_iot_mutex_delete(alipay_iot_mutex mutex) {
  return h2_pal_mutex_destroy(h2_alipay_sdk_config()->sync, mutex) == H2_PAL_OK
             ? 0
             : -1;
}
int alipay_iot_dns(const char* name, unsigned char ip[4]) {
  if (name == NULL || ip == NULL) return -1;
  h2_pal_net_addr_list_t addresses;
  uint64_t started = h2_alipay_sdk_now();
  int rc = h2_pal_net_resolve_all(h2_alipay_sdk_config()->net, name,
                                  H2_PAL_NET_FAMILY_IPV4, &addresses);
  h2_alipay_sdk_trace("dns", "resolver", rc, 0, 0, started, name, NULL, 0);
  if (rc != H2_PAL_OK || addresses.count == 0u) return -1;
  memcpy(ip, addresses.addrs[0].ip, 4u);
  char address[24];
  (void)snprintf(address, sizeof(address), "%u.%u.%u.%u", ip[0], ip[1], ip[2],
                 ip[3]);
  h2_alipay_sdk_trace("dns.address", "resolver", 0, 0, 0, started, address,
                      NULL, 0);
  return 0;
}
