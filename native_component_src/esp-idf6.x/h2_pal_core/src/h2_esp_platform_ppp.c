#include "h2_esp_platform_core.h"

#include "sdkconfig.h"

#if CONFIG_PPP_SUPPORT
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "netif/ppp/ppp.h"

typedef struct h2_esp_ppp_quiesce {
  esp_netif_t *netif;
  bool close_requested;
  bool dead;
} h2_esp_ppp_quiesce_t;

static esp_err_t ppp_quiesce_in_tcpip(void *user) {
  h2_esp_ppp_quiesce_t *request = user;
  if ((esp_netif_get_flags(request->netif) & ESP_NETIF_FLAG_IS_PPP) == 0u)
    return ESP_ERR_INVALID_ARG;
  struct netif *netif = esp_netif_get_netif_impl(request->netif);
  if (netif == NULL || netif->state == NULL) return ESP_ERR_INVALID_STATE;
  /* IDF 6.x's PPP netif state is the lwIP-owned control block. Keep that
   * SDK detail here; consumers neither inspect it nor call ppp_free(). */
  ppp_pcb *pcb = netif->state;
  if (pcb->phase != PPP_PHASE_DEAD && !request->close_requested) {
    const err_t rc = ppp_close(pcb, 0);
    if (rc != ERR_OK && rc != ERR_INPROGRESS) return ESP_FAIL;
    request->close_requested = true;
  }
  request->dead = pcb->phase == PPP_PHASE_DEAD;
  return ESP_OK;
}
#endif

h2_pal_result_t h2_esp_platform_ppp_quiesce(void *netif_handle,
                                          uint32_t timeout_ms) {
  if (netif_handle == NULL) return H2_PAL_ERR_INVALID_ARG;
#if CONFIG_PPP_SUPPORT
  h2_esp_ppp_quiesce_t request = {.netif = netif_handle};
  const int64_t started_us = esp_timer_get_time();
  const int64_t budget_us = (int64_t)timeout_ms * 1000;
  for (;;) {
    const esp_err_t rc = esp_netif_tcpip_exec(ppp_quiesce_in_tcpip, &request);
    if (rc == ESP_ERR_INVALID_ARG) return H2_PAL_ERR_INVALID_ARG;
    if (rc == ESP_ERR_INVALID_STATE) return H2_PAL_ERR_INVALID_STATE;
    if (rc != ESP_OK) return H2_PAL_ERR_IO;
    if (request.dead) return H2_PAL_OK;
    if (esp_timer_get_time() - started_us >= budget_us)
      return H2_PAL_ERR_TIMEOUT;
    /* Yield to the PPP FSM and re-read its actual phase, not an event bit
     * which can describe DISCONNECT or an earlier session. */
    vTaskDelay(1);
  }
#else
  (void)timeout_ms;
  return H2_PAL_ERR_UNSUPPORTED;
#endif
}
