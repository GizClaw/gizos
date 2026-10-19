#include "h2_esp_platform_core.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "freertos/task.h"
#include "netif/ppp/ppp.h"
#include "sdkconfig.h"

#include <assert.h>
#include <stdio.h>

static ppp_pcb s_pcb;
static struct netif s_impl = {.state = &s_pcb};
static esp_netif_t s_netif = {.flags = ESP_NETIF_FLAG_IS_PPP, .impl = &s_impl};
static int64_t s_time_us;
static unsigned s_dispatches;
static unsigned s_delays;
static unsigned s_close_calls;
static unsigned s_dead_after_delays;
static bool s_tcpip;
static bool s_dispatch_fail;
static err_t s_close_result;

esp_err_t esp_netif_tcpip_exec(esp_err_t (*callback)(void *), void *user) {
  assert(!s_tcpip);
  ++s_dispatches;
  if (s_dispatch_fail) return ESP_FAIL;
  s_tcpip = true;
  const esp_err_t rc = callback(user);
  s_tcpip = false;
  return rc;
}
esp_netif_flags_t esp_netif_get_flags(esp_netif_t *netif) {
  assert(s_tcpip);
  return netif->flags;
}
void *esp_netif_get_netif_impl(esp_netif_t *netif) {
  assert(s_tcpip);
  return netif->impl;
}
err_t ppp_close(ppp_pcb *pcb, uint8_t nocarrier) {
  assert(s_tcpip && pcb == &s_pcb && nocarrier == 0u);
  ++s_close_calls;
  if (s_close_result == ERR_OK) pcb->phase = PPP_PHASE_TERMINATE;
  return s_close_result;
}
int64_t esp_timer_get_time(void) { return s_time_us; }
void vTaskDelay(TickType_t ticks) {
  assert(!s_tcpip && ticks == 1u);
  ++s_delays;
  s_time_us += 1000;
  if (s_dead_after_delays != 0u && s_delays >= s_dead_after_delays)
    s_pcb.phase = PPP_PHASE_DEAD;
}

static void reset(void) {
  s_pcb.phase = PPP_PHASE_ESTABLISH;
  s_netif.flags = ESP_NETIF_FLAG_IS_PPP;
  s_netif.impl = &s_impl;
  s_impl.state = &s_pcb;
  s_time_us = 0;
  s_dispatches = s_delays = s_close_calls = s_dead_after_delays = 0u;
  s_tcpip = s_dispatch_fail = false;
  s_close_result = ERR_OK;
}

int main(void) {
  reset();
  assert(h2_esp_platform_ppp_quiesce(NULL, 20u) == H2_PAL_ERR_INVALID_ARG);
  assert(s_dispatches == 0u);
#if CONFIG_PPP_SUPPORT
  s_pcb.phase = PPP_PHASE_DEAD;
  assert(h2_esp_platform_ppp_quiesce(&s_netif, 0u) == H2_PAL_OK);
  assert(s_close_calls == 0u && s_delays == 0u);

  reset();
  s_dead_after_delays = 3u;
  assert(h2_esp_platform_ppp_quiesce(&s_netif, 20u) == H2_PAL_OK);
  assert(s_pcb.phase == PPP_PHASE_DEAD && s_delays == 3u);
  assert(s_close_calls == 1u);

  /* An earlier DISCONNECT notification or a cleared modem PPP_STARTED bit
   * cannot authorize freeing a live PCB. The actual phase still wins. */
  reset();
  s_pcb.phase = PPP_PHASE_DISCONNECT;
  s_close_result = ERR_INPROGRESS;
  assert(h2_esp_platform_ppp_quiesce(&s_netif, 2u) == H2_PAL_ERR_TIMEOUT);
  assert(s_pcb.phase == PPP_PHASE_DISCONNECT && s_close_calls == 1u);
  s_pcb.phase = PPP_PHASE_DEAD;
  assert(h2_esp_platform_ppp_quiesce(&s_netif, 0u) == H2_PAL_OK);

  reset();
  assert(h2_esp_platform_ppp_quiesce(&s_netif, 0u) == H2_PAL_ERR_TIMEOUT);
  assert(s_close_calls == 1u && s_delays == 0u);

  reset();
  s_dispatch_fail = true;
  assert(h2_esp_platform_ppp_quiesce(&s_netif, 20u) == H2_PAL_ERR_IO);
  assert(s_close_calls == 0u && s_delays == 0u);

  reset();
  s_close_result = -1;
  assert(h2_esp_platform_ppp_quiesce(&s_netif, 20u) == H2_PAL_ERR_IO);
  assert(s_close_calls == 1u && s_delays == 0u);

  reset();
  s_netif.flags = 0u;
  assert(h2_esp_platform_ppp_quiesce(&s_netif, 20u) == H2_PAL_ERR_INVALID_ARG);
  reset();
  s_netif.impl = NULL;
  assert(h2_esp_platform_ppp_quiesce(&s_netif, 20u) == H2_PAL_ERR_INVALID_STATE);
  reset();
  s_impl.state = NULL;
  assert(h2_esp_platform_ppp_quiesce(&s_netif, 20u) == H2_PAL_ERR_INVALID_STATE);
#else
  assert(h2_esp_platform_ppp_quiesce(&s_netif, 20u) == H2_PAL_ERR_UNSUPPORTED);
  assert(s_dispatches == 0u && s_close_calls == 0u && s_delays == 0u);
#endif
  puts("PPP quiescence contract: PASS");
  return 0;
}
