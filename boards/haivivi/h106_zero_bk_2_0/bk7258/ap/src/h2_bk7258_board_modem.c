#if defined(H2_ZERO_BK_MODEM_HOST_TEST)
#include "h2_zero_bk_modem_test_sdk.h"
#else
#include "h2_bk7258_board_private.h"
#include "h2_zero_bk_board_config.h"

#include "h2_bk_platform_core.h"
#include "h2_apn.h"
#include "h2_quectel_modem.h"

#include "driver/gpio.h"
#include "driver/pwr_clk.h"
#include "driver/uart.h"
#include "gpio_driver.h"
#include "gpio_map.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "os/os.h"
#endif

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define H2_ZERO_BK_MODEM_UART ((uart_id_t)H2_ZERO_BK_MODEM_UART_ID)
#define H2_ZERO_BK_MODEM_BOOT_MS 5000u
#define H2_ZERO_BK_MODEM_POWER_OFF_MS 1000u
#define H2_ZERO_BK_MODEM_POWER_SETTLE_MS 200u
#define H2_ZERO_BK_MODEM_AT_READ_MS 8000u
#define H2_ZERO_BK_MODEM_PPP_POLL_MS 20u
#define H2_ZERO_BK_MODEM_PPP_STACK_SIZE 4096u
#define H2_ZERO_BK_MODEM_PPP_PRIORITY (BEKEN_DEFAULT_WORKER_PRIORITY - 1u)
#define H2_ZERO_BK_MODEM_DEFAULT_TIMEOUT_MS 45000u

static h2_quectel_modem_t s_modem;
static h2_pal_modem_vtable_t s_modem_vtable;
static const h2_pal_modem_vtable_t* s_quectel_vtable;
static h2_pal_modem_status_t s_cached_status;
static h2_pal_modem_signal_t s_ppp_signal;
static h2_pal_modem_operator_t s_cached_operator;
static struct netif s_ppp_netif;
static ppp_pcb* s_ppp;
static beken_thread_t s_ppp_rx_thread;
static beken_semaphore_t s_ppp_event;
static bool s_modem_initialized;
static bool s_modem_powered;
static bool s_uart_open;
static bool s_cached_status_valid;
static bool s_cached_operator_valid;
static atomic_bool s_ppp_rx_running;
static atomic_bool s_ppp_close_requested;
static atomic_bool s_ppp_terminal;
static bool s_ppp_connect_started;
static bool s_ppp_close_sent;
static bool s_ppp_command_unconfirmed;

extern uint32_t uart_get_length_in_buffer(uart_id_t id);

static void modem_post_event(h2_pal_system_event_type_t type,
                             const void* payload, size_t payload_size) {
  h2_pal_system_event_t event = {
      .type = type,
      .payload = payload,
      .payload_size = payload_size,
  };

  (void)h2_pal_system_event_post(h2_bk7258_board_system_event_api(), &event,
                                 0u);
}

static h2_pal_result_t map_uart_result(int result) {
  if (result >= 0) {
    return H2_PAL_OK;
  }
  if (result == BK_ERR_UART_RX_TIMEOUT) {
    return H2_PAL_ERR_TIMEOUT;
  }
  return H2_PAL_ERR_IO;
}

static h2_pal_result_t modem_gpio_output(gpio_id_t gpio, bool high) {
  gpio_config_t config = {
      .io_mode = GPIO_OUTPUT_ENABLE,
      .pull_mode = GPIO_PULL_DISABLE,
      .func_mode = GPIO_SECOND_FUNC_DISABLE,
  };

  (void)gpio_dev_unmap(gpio);
  if (bk_gpio_set_config(gpio, &config) != BK_OK) {
    return H2_PAL_ERR_IO;
  }
  if ((high ? bk_gpio_set_output_high(gpio) : bk_gpio_set_output_low(gpio)) !=
      BK_OK) {
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t modem_uart_apply_pinmux(void) {
  (void)gpio_dev_unmap((gpio_id_t)H2_ZERO_BK_MODEM_UART_TX_GPIO);
  (void)gpio_dev_unmap((gpio_id_t)H2_ZERO_BK_MODEM_UART_RX_GPIO);
  const bk_err_t tx_rc = gpio_dev_map((gpio_id_t)H2_ZERO_BK_MODEM_UART_TX_GPIO,
                                      GPIO_DEV_UART2_TXD);
  const bk_err_t rx_rc = gpio_dev_map((gpio_id_t)H2_ZERO_BK_MODEM_UART_RX_GPIO,
                                      GPIO_DEV_UART2_RXD);
  (void)bk_gpio_pull_up((gpio_id_t)H2_ZERO_BK_MODEM_UART_TX_GPIO);
  (void)bk_gpio_pull_up((gpio_id_t)H2_ZERO_BK_MODEM_UART_RX_GPIO);
  printf("H2_ZERO_BK_MODEM_PINMUX tx_rc=%d rx_rc=%d tx=%08lx rx=%08lx\r\n",
         (int)tx_rc, (int)rx_rc,
         (unsigned long)bk_gpio_get_value(
             (gpio_id_t)H2_ZERO_BK_MODEM_UART_TX_GPIO),
         (unsigned long)bk_gpio_get_value(
             (gpio_id_t)H2_ZERO_BK_MODEM_UART_RX_GPIO));
  return tx_rc == BK_OK && rx_rc == BK_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t modem_transport_flush(void* user);

static h2_pal_result_t modem_transport_init(void* user) {
  bool wait_for_boot = false;
  uart_config_t config = {
      .baud_rate = H2_ZERO_BK_MODEM_UART_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_NONE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_FLOWCTRL_DISABLE,
      .src_clk = UART_SCLK_XTAL_26M,
  };

  (void)user;
  if (!s_modem_powered) {
    if (h2_zero_bk_peripheral_power_enable() != H2_PAL_OK) {
      printf("H2_ZERO_BK_MODEM_POWER_FAIL stage=peripheral_ldo\r\n");
      return H2_PAL_ERR_IO;
    }
    rtos_delay_milliseconds(50u);
    if (modem_gpio_output((gpio_id_t)H2_ZERO_BK_MODEM_ENABLE_GPIO, false) !=
        H2_PAL_OK) {
      printf("H2_ZERO_BK_MODEM_POWER_FAIL stage=enable_low\r\n");
      return H2_PAL_ERR_IO;
    }
    rtos_delay_milliseconds(H2_ZERO_BK_MODEM_POWER_OFF_MS);
    if (modem_gpio_output((gpio_id_t)H2_ZERO_BK_MODEM_ENABLE_GPIO, true) !=
        H2_PAL_OK) {
      printf("H2_ZERO_BK_MODEM_POWER_FAIL stage=enable_high\r\n");
      return H2_PAL_ERR_IO;
    }
    s_modem_powered = true;
    wait_for_boot = true;
  } else {
    if (modem_gpio_output((gpio_id_t)H2_ZERO_BK_MODEM_ENABLE_GPIO, true) !=
        H2_PAL_OK) {
      printf("H2_ZERO_BK_MODEM_POWER_FAIL stage=enable_restore\r\n");
      return H2_PAL_ERR_IO;
    }
  }
  rtos_delay_milliseconds(H2_ZERO_BK_MODEM_POWER_SETTLE_MS);

  if (s_uart_open) {
    if (wait_for_boot) {
      rtos_delay_milliseconds(H2_ZERO_BK_MODEM_BOOT_MS -
                              H2_ZERO_BK_MODEM_POWER_SETTLE_MS);
    }
    return H2_PAL_OK;
  }
  (void)bk_uart_deinit(H2_ZERO_BK_MODEM_UART);
  if (bk_uart_init(H2_ZERO_BK_MODEM_UART, &config) != BK_OK) {
    return H2_PAL_ERR_IO;
  }
  /* SDK init starts RX on its default pins. Keep reception disabled while
   * selecting this board's pins, then discard only pre-session RX data. */
  if (bk_uart_set_enable_rx(H2_ZERO_BK_MODEM_UART, false) != BK_OK) {
    (void)bk_uart_deinit(H2_ZERO_BK_MODEM_UART);
    return H2_PAL_ERR_IO;
  }
  (void)bk_uart_set_enable_tx(H2_ZERO_BK_MODEM_UART, true);
  (void)bk_uart_set_rx_timeout(H2_ZERO_BK_MODEM_UART,
                               UART_RX_STOP_DETECT_TIME_256_BITS);
  (void)bk_uart_enable_rx_interrupt(H2_ZERO_BK_MODEM_UART);
  if (modem_uart_apply_pinmux() != H2_PAL_OK) {
    (void)bk_uart_deinit(H2_ZERO_BK_MODEM_UART);
    return H2_PAL_ERR_IO;
  }
  (void)modem_transport_flush(NULL);
  if (bk_uart_set_enable_rx(H2_ZERO_BK_MODEM_UART, true) != BK_OK) {
    (void)bk_uart_deinit(H2_ZERO_BK_MODEM_UART);
    return H2_PAL_ERR_IO;
  }
  s_uart_open = true;
  printf("H2_ZERO_BK_MODEM_UART_READY id=%u baud=%u rx=%u tx=%u\r\n",
         (unsigned)H2_ZERO_BK_MODEM_UART_ID,
         (unsigned)H2_ZERO_BK_MODEM_UART_BAUD,
         (unsigned)H2_ZERO_BK_MODEM_UART_RX_GPIO,
         (unsigned)H2_ZERO_BK_MODEM_UART_TX_GPIO);
  if (wait_for_boot) {
    rtos_delay_milliseconds(H2_ZERO_BK_MODEM_BOOT_MS -
                            H2_ZERO_BK_MODEM_POWER_SETTLE_MS);
  }
  return H2_PAL_OK;
}

static h2_pal_result_t modem_transport_deinit(void* user) {
  (void)user;
  if (s_ppp != NULL || s_ppp_rx_thread != NULL || s_ppp_command_unconfirmed) {
    return H2_PAL_ERR_BUSY;
  }
  if (!s_uart_open) {
    return H2_PAL_OK;
  }
  h2_pal_result_t result = h2_quectel_modem_transport_closing(&s_modem);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (bk_uart_deinit(H2_ZERO_BK_MODEM_UART) != BK_OK) {
    return H2_PAL_ERR_IO;
  }
  s_uart_open = false;
  return H2_PAL_OK;
}

static h2_pal_result_t modem_transport_flush(void* user) {
  uint8_t discard[64];

  (void)user;
  for (;;) {
    uint32_t available = uart_get_length_in_buffer(H2_ZERO_BK_MODEM_UART);
    if (available == 0u) {
      break;
    }
    uint32_t read_size =
        available < sizeof(discard) ? available : sizeof(discard);
    if (bk_uart_read_bytes(H2_ZERO_BK_MODEM_UART, discard, read_size, 0u) <=
        0) {
      break;
    }
  }
  return H2_PAL_OK;
}

static h2_pal_result_t modem_transport_read(void* user, uint8_t* buf,
                                            size_t len, uint32_t timeout_ms,
                                            size_t* out_len) {
  int result;

  (void)user;
  if (buf == NULL || len == 0u || len > UINT32_MAX || out_len == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_len = 0u;
  result =
      bk_uart_read_bytes(H2_ZERO_BK_MODEM_UART, buf, (uint32_t)len, timeout_ms);
  static unsigned read_logs;
  if (read_logs++ < 64u) {
    printf(
        "H2_ZERO_BK_MODEM_RX result=%d requested=%lu timeout=%lu "
        "first=%02x\r\n",
        result, (unsigned long)len, (unsigned long)timeout_ms,
        result > 0 ? (unsigned)buf[0] : 0u);
  }
  if (result >= 0) {
    *out_len = (size_t)result;
  }
  return map_uart_result(result);
}

static h2_pal_result_t modem_transport_write(void* user, const uint8_t* buf,
                                             size_t len, uint32_t timeout_ms,
                                             size_t* out_len) {
  (void)user;
  (void)timeout_ms;
  if ((buf == NULL && len != 0u) || len > UINT32_MAX || out_len == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_len = 0u;
  if (bk_uart_write_bytes(H2_ZERO_BK_MODEM_UART, buf, (uint32_t)len) != BK_OK) {
    return H2_PAL_ERR_IO;
  }
  if (len != 0u && buf[len - 1u] == '\r') {
    static const uint8_t line_feed = '\n';

    if (bk_uart_write_bytes(H2_ZERO_BK_MODEM_UART, &line_feed, 1u) != BK_OK) {
      return H2_PAL_ERR_IO;
    }
  }
  bk_uart_wait_tx_over(H2_ZERO_BK_MODEM_UART);
  *out_len = len;
  return H2_PAL_OK;
}

static void modem_ppp_report_netif_change(void) {
  (void)h2_bk_platform_netif_reconcile_default_async();
}

static h2_pal_result_t modem_state_lock(void) {
  return s_modem.lock != NULL
             ? h2_pal_mutex_lock(s_modem.config.sync_api, s_modem.lock)
             : H2_PAL_OK;
}

static void modem_state_unlock(void) {
  if (s_modem.lock != NULL)
    (void)h2_pal_mutex_unlock(s_modem.config.sync_api, s_modem.lock);
}

static h2_pal_modem_data_status_t modem_data_snapshot(void) {
  h2_pal_modem_data_status_t status = {0};
  if (modem_state_lock() == H2_PAL_OK) {
    status = s_modem.data_status;
    modem_state_unlock();
  } else {
    status.state = H2_PAL_MODEM_DATA_CLOSING;
    status.last_error = H2_PAL_ERR_IO;
  }
  return status;
}

static void modem_set_data_state(h2_pal_modem_data_state_t state,
                                 h2_pal_result_t result) {
  if (modem_state_lock() != H2_PAL_OK) return;
  s_modem.data_status.state = state;
  s_modem.data_status.last_error = result;
  if (state != H2_PAL_MODEM_DATA_OPEN) {
    s_modem.data_status.ip4 = 0u;
    s_modem.data_status.dns1_ip4 = 0u;
    s_modem.data_status.dns2_ip4 = 0u;
    s_modem.data_status.ip4_valid = 0u;
  }
  modem_state_unlock();
}

static void modem_ppp_mark_closed(h2_pal_result_t result, bool post_event) {
  modem_set_data_state(H2_PAL_MODEM_DATA_CLOSED, result);
  if (post_event) {
    h2_pal_modem_data_status_t status = modem_data_snapshot();
    modem_post_event(H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_CLOSED, &status,
                     sizeof(status));
  }
}

static u32_t modem_ppp_output(ppp_pcb* pcb, u8_t* data, u32_t len, void* ctx) {
  (void)pcb;
  (void)ctx;
  return bk_uart_write_bytes(H2_ZERO_BK_MODEM_UART, data, len) == BK_OK ? len
                                                                        : 0u;
}

static void modem_ppp_status(ppp_pcb* pcb, int error, void* ctx) {
  (void)pcb;
  (void)ctx;
  h2_pal_modem_data_status_t status = {0};
  if (modem_state_lock() != H2_PAL_OK) return;
  if (error == PPPERR_NONE && !s_ppp_close_requested && !s_ppp_terminal) {
    const ip_addr_t* dns1 = dns_getserver(0u);
    const ip_addr_t* dns2 = dns_getserver(1u);
    s_modem.data_status.state = H2_PAL_MODEM_DATA_OPEN;
    s_modem.data_status.ip4 = ip4_addr_get_u32(netif_ip4_addr(&s_ppp_netif));
    s_modem.data_status.dns1_ip4 =
        dns1 != NULL ? ip4_addr_get_u32(ip_2_ip4(dns1)) : 0u;
    s_modem.data_status.dns2_ip4 =
        dns2 != NULL ? ip4_addr_get_u32(ip_2_ip4(dns2)) : 0u;
    s_modem.data_status.ip4_valid = s_modem.data_status.ip4 != 0u ? 1u : 0u;
    s_modem.data_status.last_error = H2_PAL_OK;
    status = s_modem.data_status;
    modem_state_unlock();
    h2_bk_platform_netif_register(&s_ppp_netif, H2_PAL_NETIF_KIND_MODEM_DATA);
    modem_ppp_report_netif_change();
    modem_post_event(H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_OPENED, &status,
                     sizeof(status));
  } else {
    if (error != PPPERR_NONE) {
      s_modem.data_status.state = H2_PAL_MODEM_DATA_CLOSING;
      s_modem.data_status.ip4 = 0u;
      s_modem.data_status.dns1_ip4 = 0u;
      s_modem.data_status.dns2_ip4 = 0u;
      s_modem.data_status.ip4_valid = 0u;
      s_modem.data_status.last_error =
          s_ppp_close_requested ? H2_PAL_OK : H2_PAL_ERR_IO;
    }
    modem_state_unlock();
    if (error != PPPERR_NONE) {
      s_ppp_terminal = true;
      modem_ppp_report_netif_change();
    }
  }
  if (s_ppp_event != NULL) (void)rtos_set_semaphore(&s_ppp_event);
}

static void modem_ppp_rx_main(void* arg) {
  uint8_t buf[512];

  (void)arg;
  while (s_ppp_rx_running) {
    uint32_t available = uart_get_length_in_buffer(H2_ZERO_BK_MODEM_UART);
    if (available == 0u) {
      rtos_delay_milliseconds(H2_ZERO_BK_MODEM_PPP_POLL_MS);
      continue;
    }
    uint32_t read_size = available < sizeof(buf) ? available : sizeof(buf);
    int result = bk_uart_read_bytes(H2_ZERO_BK_MODEM_UART, buf, read_size, 0u);
    if (result > 0 && s_ppp != NULL) {
      (void)pppos_input_tcpip(s_ppp, buf, result);
    } else if (result < 0 && result != BK_ERR_UART_RX_TIMEOUT) {
      printf("H2_ZERO_BK_MODEM_PPP_RX_FAIL rc=%d\r\n", result);
      break;
    }
  }
  s_ppp_rx_running = false;
  rtos_delete_thread(NULL);
}

static h2_pal_result_t modem_wait_registered(uint32_t timeout_ms) {
  const uint32_t effective_timeout =
      timeout_ms == 0u ? H2_ZERO_BK_MODEM_DEFAULT_TIMEOUT_MS : timeout_ms;
  const uint32_t started_ms = (uint32_t)rtos_get_time();

  while (true) {
    uint32_t elapsed_ms = (uint32_t)((uint32_t)rtos_get_time() - started_ms);
    uint32_t remaining_ms;
    uint32_t previous_command_timeout_ms;
    uint32_t command_timeout_ms;
    h2_pal_modem_status_t status = {0};

    if (elapsed_ms >= effective_timeout) {
      break;
    }
    remaining_ms = effective_timeout - elapsed_ms;
    previous_command_timeout_ms = s_modem.config.command_timeout_ms;
    command_timeout_ms = remaining_ms / 4u;
    if (command_timeout_ms < 250u) {
      command_timeout_ms = remaining_ms < 250u ? remaining_ms : 250u;
    }
    if (command_timeout_ms > previous_command_timeout_ms) {
      command_timeout_ms = previous_command_timeout_ms;
    }
    s_modem.config.command_timeout_ms = command_timeout_ms;
    h2_pal_result_t result =
        s_quectel_vtable->get_status(s_modem.platform.user, &status);
    s_modem.config.command_timeout_ms = previous_command_timeout_ms;

    if (result == H2_PAL_OK) {
      s_cached_status = status;
      s_cached_status_valid = true;
    }
    if (result == H2_PAL_OK &&
        (status.registration == H2_PAL_MODEM_REGISTRATION_HOME ||
         status.registration == H2_PAL_MODEM_REGISTRATION_ROAMING)) {
      return H2_PAL_OK;
    }
    elapsed_ms = (uint32_t)((uint32_t)rtos_get_time() - started_ms);
    if (elapsed_ms >= effective_timeout) {
      break;
    }
    remaining_ms = effective_timeout - elapsed_ms;
    rtos_delay_milliseconds(remaining_ms < 1000u ? remaining_ms : 1000u);
  }
  return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t modem_configure_apn(void) {
  h2_pal_modem_apn_config_t config = {0};

  if (s_modem.last_apn[0] != '\0') {
    return H2_PAL_OK;
  }
  h2_pal_modem_identity_t identity = {0};
  h2_pal_result_t result =
      s_quectel_vtable->get_identity(s_modem.platform.user, &identity);
  const char* apn =
      result == H2_PAL_OK ? h2_apn_from_imsi(identity.imsi) : "internet";

  (void)strncpy(config.apn, apn, sizeof(config.apn) - 1u);
  printf("H2_ZERO_BK_MODEM_APN value=%s\r\n", config.apn);
  return h2_quectel_modem_set_apn(&s_modem.platform, &config);
}

static h2_pal_modem_t* modem_from_user(void* user) {
  h2_quectel_modem_t* modem = (h2_quectel_modem_t*)user;
  return modem != NULL ? &modem->platform : NULL;
}

static h2_pal_result_t modem_data_close(h2_pal_modem_t* platform,
                                        uint32_t timeout_ms);

static h2_pal_result_t modem_open_adapter_impl(void* user,
                                               uint32_t timeout_ms) {
  if (s_ppp != NULL || s_ppp_rx_thread != NULL || s_ppp_command_unconfirmed)
    return H2_PAL_ERR_BUSY;
  h2_pal_result_t result = s_quectel_vtable->open(user, timeout_ms);
  static unsigned failure_logs;
  if (result != H2_PAL_OK && failure_logs++ < 8u) {
    printf("H2_ZERO_BK_MODEM_OPEN rc=%d\r\n", (int)result);
  }
  return result;
}

static bool modem_ppp_retained(void) {
  return s_ppp != NULL || s_ppp_rx_thread != NULL || s_ppp_command_unconfirmed;
}

static h2_pal_result_t modem_check_ota(void) {
  h2_pal_result_t result = modem_state_lock();
  if (result != H2_PAL_OK) return result;
  bool busy = s_modem.ota_hold != 0u;
  modem_state_unlock();
  return busy ? H2_PAL_ERR_BUSY : H2_PAL_OK;
}

static h2_pal_result_t modem_close_adapter_impl(void* user,
                                                uint32_t timeout_ms) {
  h2_pal_result_t admission = modem_check_ota();
  if (admission != H2_PAL_OK) return admission;
  if (modem_ppp_retained()) {
    h2_pal_result_t result = modem_data_close(&s_modem.platform, timeout_ms);
    if (result != H2_PAL_OK) return result;
  }
  return s_quectel_vtable->close(user, timeout_ms);
}

static h2_pal_result_t modem_data_open(h2_pal_modem_t* platform,
                                       uint32_t timeout_ms) {
  const uint32_t effective_timeout =
      timeout_ms == 0u ? H2_ZERO_BK_MODEM_DEFAULT_TIMEOUT_MS : timeout_ms;
  h2_pal_result_t admission = modem_check_ota();
  if (admission != H2_PAL_OK) return admission;
  h2_pal_modem_data_status_t status = modem_data_snapshot();
  if (status.state == H2_PAL_MODEM_DATA_OPEN && status.ip4_valid != 0u &&
      !s_ppp_terminal) {
    return H2_PAL_OK;
  }
  h2_pal_result_t result;
  if (modem_ppp_retained()) {
    result = modem_data_close(platform, effective_timeout);
    if (result != H2_PAL_OK) return result;
  }
  result = s_quectel_vtable->open(platform->user, effective_timeout);
  if (result != H2_PAL_OK) return result;
  result = modem_configure_apn();
  if (result == H2_PAL_OK) {
    result = modem_wait_registered(effective_timeout);
  }
  if (result == H2_PAL_OK) {
    result = s_quectel_vtable->get_signal(platform->user, &s_ppp_signal);
  }
  if (result == H2_PAL_OK) {
    h2_pal_modem_operator_t modem_operator = {0};

    if (s_quectel_vtable->get_operator(platform->user, &modem_operator) ==
        H2_PAL_OK) {
      s_cached_operator = modem_operator;
      s_cached_operator_valid = true;
    }
  }
  if (result != H2_PAL_OK) {
    modem_ppp_mark_closed(result, false);
    return result;
  }

  h2_bk_platform_netif_unregister(&s_ppp_netif);
  memset(&s_ppp_netif, 0, sizeof(s_ppp_netif));
  while (rtos_get_semaphore(&s_ppp_event, 0u) == kNoErr) {
  }
  s_ppp = pppapi_pppos_create(&s_ppp_netif, modem_ppp_output, modem_ppp_status,
                              NULL);
  if (s_ppp == NULL) {
    modem_ppp_mark_closed(H2_PAL_ERR_NO_MEMORY, false);
    return H2_PAL_ERR_NO_MEMORY;
  }
  ppp_set_usepeerdns(s_ppp, 1);
  if (s_modem.last_username[0] != '\0' || s_modem.last_password[0] != '\0') {
    ppp_set_auth(s_ppp, PPPAUTHTYPE_PAP | PPPAUTHTYPE_CHAP,
                 s_modem.last_username, s_modem.last_password);
  }
  modem_set_data_state(H2_PAL_MODEM_DATA_OPENING, H2_PAL_OK);
  s_ppp_close_requested = false;
  s_ppp_terminal = false;
  s_ppp_connect_started = false;
  s_ppp_close_sent = false;
  /* A failed dial can still have switched the modem into data mode. */
  s_ppp_command_unconfirmed = true;

  result = h2_quectel_modem_dial_ppp(&s_modem);
  /* A failed dial can be ambiguous once it has reached the transport. */
  h2_pal_result_t gate = h2_quectel_modem_set_raw_ppp_owned(&s_modem, 1);
  if (gate != H2_PAL_OK) return gate;
  if (result == H2_PAL_OK) {
    s_ppp_rx_running = true;
    if (rtos_core0_create_psram_thread(
            &s_ppp_rx_thread, H2_ZERO_BK_MODEM_PPP_PRIORITY, "h2_pppos_rx",
            modem_ppp_rx_main, H2_ZERO_BK_MODEM_PPP_STACK_SIZE,
            NULL) != kNoErr) {
      s_ppp_rx_running = false;
      result = H2_PAL_ERR_NO_MEMORY;
    } else {
      printf(
          "H2_PAL_TASK_READY name=h2_pppos_rx core=0 priority=5 "
          "stack=psram size=4096\r\n");
    }
  }
  if (result == H2_PAL_OK) {
    s_ppp_connect_started = true;
    if (pppapi_connect(s_ppp, 0u) != ERR_OK) {
      s_ppp_connect_started = false;
      result = H2_PAL_ERR_IO;
    }
  }
  if (result == H2_PAL_OK &&
      rtos_get_semaphore(&s_ppp_event, effective_timeout) != kNoErr) {
    result = H2_PAL_ERR_TIMEOUT;
  }
  status = modem_data_snapshot();
  if (result == H2_PAL_OK &&
      (status.state != H2_PAL_MODEM_DATA_OPEN || status.ip4_valid == 0u)) {
    result = status.last_error != H2_PAL_OK ? status.last_error : H2_PAL_ERR_IO;
  }
  if (result != H2_PAL_OK) {
    (void)modem_data_close(platform, 2000u);
  }
  return result;
}

static h2_pal_result_t modem_data_open_adapter_impl(void* user,
                                                    uint32_t timeout_ms) {
  return modem_data_open(modem_from_user(user), timeout_ms);
}

static h2_pal_result_t modem_data_close(h2_pal_modem_t* platform,
                                        uint32_t timeout_ms) {
  (void)platform;
  const uint32_t effective_timeout = timeout_ms == 0u ? 3000u : timeout_ms;

  h2_pal_result_t admission = modem_check_ota();
  if (admission != H2_PAL_OK) return admission;
  if (!modem_ppp_retained()) return H2_PAL_OK;
  s_ppp_close_requested = true;
  modem_set_data_state(H2_PAL_MODEM_DATA_CLOSING, H2_PAL_OK);
  h2_pal_result_t result = H2_PAL_OK;
  if (s_ppp != NULL && s_ppp_connect_started && !s_ppp_terminal) {
    while (rtos_get_semaphore(&s_ppp_event, 0u) == kNoErr) {
    }
    if (!s_ppp_close_sent && !s_ppp_terminal) {
      if (pppapi_close(s_ppp, 0u) != ERR_OK) {
        result = H2_PAL_ERR_IO;
        goto failed;
      }
      s_ppp_close_sent = true;
    }
    if (!s_ppp_terminal &&
        rtos_get_semaphore(&s_ppp_event, effective_timeout) != kNoErr) {
      result = H2_PAL_ERR_TIMEOUT;
      goto failed;
    }
    if (!s_ppp_terminal) {
      result = H2_PAL_ERR_TIMEOUT;
      goto failed;
    }
  }
  s_ppp_rx_running = false;
  if (s_ppp_rx_thread != NULL) {
    if (rtos_thread_join(&s_ppp_rx_thread) != kNoErr) {
      result = H2_PAL_ERR_IO;
      goto failed;
    }
    s_ppp_rx_thread = NULL;
  }
  if (s_ppp != NULL) {
    if (pppapi_free(s_ppp) != ERR_OK) {
      result = H2_PAL_ERR_IO;
      goto failed;
    }
    s_ppp = NULL;
    s_ppp_connect_started = false;
  }
  modem_ppp_report_netif_change();
  h2_bk_platform_netif_unregister(&s_ppp_netif);

  if (s_ppp_command_unconfirmed) {
    result = h2_quectel_modem_set_raw_ppp_owned(&s_modem, 0);
    if (result != H2_PAL_OK) goto failed;
    rtos_delay_milliseconds(1100u);
    static const uint8_t escape[] = "+++";
    size_t written = 0u;
    result = modem_transport_write(NULL, escape, sizeof(escape) - 1u, 1000u,
                                   &written);
    if (result != H2_PAL_OK) goto reclaim;
    if (written != sizeof(escape) - 1u) {
      result = H2_PAL_ERR_IO;
      goto reclaim;
    }
    rtos_delay_milliseconds(1100u);
    result = h2_quectel_modem_drop_ppp(&s_modem);
    if (result != H2_PAL_OK) goto reclaim;
    s_ppp_command_unconfirmed = false;
  }
  s_ppp_close_requested = false;
  modem_ppp_mark_closed(H2_PAL_OK, true);
  return H2_PAL_OK;

reclaim: {
  h2_pal_result_t gate_result = h2_quectel_modem_set_raw_ppp_owned(&s_modem, 1);
  if (gate_result != H2_PAL_OK) result = gate_result;
}
failed:
  modem_set_data_state(H2_PAL_MODEM_DATA_CLOSING, result);
  return result;
}

static h2_pal_result_t modem_data_close_adapter_impl(void* user,
                                                     uint32_t timeout_ms) {
  return modem_data_close(modem_from_user(user), timeout_ms);
}

static h2_pal_result_t modem_open_for_query(void* user, const char* operation) {
  if (modem_ppp_retained()) return H2_PAL_ERR_BUSY;
  h2_pal_result_t result =
      s_quectel_vtable->open(user, H2_ZERO_BK_MODEM_DEFAULT_TIMEOUT_MS);

  if (result != H2_PAL_OK) {
    printf("H2_ZERO_BK_MODEM_QUERY_FAIL operation=%s stage=open rc=%d\r\n",
           operation, result);
  }
  return result;
}

static h2_pal_result_t modem_get_identity_adapter_impl(
    void* user, h2_pal_modem_identity_t* out_identity) {
  if (out_identity == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t result = modem_open_for_query(user, "identity");

  if (result != H2_PAL_OK) {
    return result;
  }
  result = s_quectel_vtable->get_identity(user, out_identity);
  if (result != H2_PAL_OK) {
    printf(
        "H2_ZERO_BK_MODEM_QUERY_FAIL operation=identity stage=query "
        "rc=%d\r\n",
        result);
  }
  return result;
}

static h2_pal_result_t modem_get_status_adapter_impl(
    void* user, h2_pal_modem_status_t* out_status) {
  if (out_status == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (modem_ppp_retained()) {
    if (!s_cached_status_valid) {
      return H2_PAL_ERR_UNAVAILABLE;
    }
    *out_status = s_cached_status;
    h2_pal_modem_data_state_t state = modem_data_snapshot().state;
    out_status->packet =
        state == H2_PAL_MODEM_DATA_OPEN      ? H2_PAL_MODEM_PACKET_CONNECTED
        : state == H2_PAL_MODEM_DATA_OPENING ? H2_PAL_MODEM_PACKET_CONNECTING
                                             : H2_PAL_MODEM_PACKET_UNKNOWN;
    return H2_PAL_OK;
  }
  h2_pal_result_t result = modem_open_for_query(user, "status");

  if (result != H2_PAL_OK) {
    return result;
  }
  result = s_quectel_vtable->get_status(user, out_status);
  if (result != H2_PAL_OK) {
    printf(
        "H2_ZERO_BK_MODEM_QUERY_FAIL operation=status stage=query "
        "rc=%d\r\n",
        result);
  } else {
    s_cached_status = *out_status;
    s_cached_status_valid = true;
  }
  return result;
}

static h2_pal_result_t modem_get_operator_adapter_impl(
    void* user, h2_pal_modem_operator_t* out_operator) {
  if (out_operator == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (modem_ppp_retained()) {
    if (!s_cached_operator_valid) {
      return H2_PAL_ERR_UNAVAILABLE;
    }
    *out_operator = s_cached_operator;
    return H2_PAL_OK;
  }
  h2_pal_result_t result = modem_open_for_query(user, "operator");

  if (result != H2_PAL_OK) {
    return result;
  }
  result = s_quectel_vtable->get_operator(user, out_operator);
  if (result != H2_PAL_OK) {
    printf(
        "H2_ZERO_BK_MODEM_QUERY_FAIL operation=operator stage=query "
        "rc=%d\r\n",
        result);
  } else {
    s_cached_operator = *out_operator;
    s_cached_operator_valid = true;
  }
  return result;
}

static h2_pal_result_t modem_get_signal_adapter_impl(
    void* user, h2_pal_modem_signal_t* out_signal) {
  if (out_signal == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (modem_data_snapshot().state == H2_PAL_MODEM_DATA_OPEN &&
      !s_ppp_terminal) {
    *out_signal = s_ppp_signal;
    return H2_PAL_OK;
  }
  h2_pal_result_t result = modem_open_for_query(user, "signal");

  if (result != H2_PAL_OK) {
    return result;
  }
  result = s_quectel_vtable->get_signal(user, out_signal);
  if (result != H2_PAL_OK) {
    printf(
        "H2_ZERO_BK_MODEM_QUERY_FAIL operation=signal stage=query "
        "rc=%d\r\n",
        result);
  }
  return result;
}

/* The provider operation mutex is recursive, so the underlying vtable may
 * acquire it again. Callbacks use only the state mutex. */
static h2_pal_result_t modem_operation_lock(void) {
  return s_modem.operation_lock != NULL
             ? h2_pal_mutex_lock(s_modem.config.sync_api,
                                 s_modem.operation_lock)
             : H2_PAL_OK;
}

static h2_pal_result_t modem_operation_unlock(h2_pal_result_t result) {
  if (s_modem.operation_lock != NULL) {
    h2_pal_result_t unlock =
        h2_pal_mutex_unlock(s_modem.config.sync_api, s_modem.operation_lock);
    if (result == H2_PAL_OK) result = unlock;
  }
  return result;
}

static h2_pal_result_t modem_open_adapter(void* user, uint32_t timeout_ms) {
  h2_pal_result_t result = modem_operation_lock();
  if (result != H2_PAL_OK) return result;
  return modem_operation_unlock(modem_open_adapter_impl(user, timeout_ms));
}

static h2_pal_result_t modem_close_adapter(void* user, uint32_t timeout_ms) {
  h2_pal_result_t result = modem_operation_lock();
  if (result != H2_PAL_OK) return result;
  return modem_operation_unlock(modem_close_adapter_impl(user, timeout_ms));
}

static h2_pal_result_t modem_data_open_adapter(void* user,
                                               uint32_t timeout_ms) {
  h2_pal_result_t result = modem_operation_lock();
  if (result != H2_PAL_OK) return result;
  return modem_operation_unlock(modem_data_open_adapter_impl(user, timeout_ms));
}

static h2_pal_result_t modem_data_close_adapter(void* user,
                                                uint32_t timeout_ms) {
  h2_pal_result_t result = modem_operation_lock();
  if (result != H2_PAL_OK) return result;
  return modem_operation_unlock(
      modem_data_close_adapter_impl(user, timeout_ms));
}

static h2_pal_result_t modem_get_identity_adapter(
    void* user, h2_pal_modem_identity_t* out) {
  h2_pal_result_t result = modem_operation_lock();
  if (result != H2_PAL_OK) return result;
  return modem_operation_unlock(modem_get_identity_adapter_impl(user, out));
}

static h2_pal_result_t modem_get_status_adapter(void* user,
                                                h2_pal_modem_status_t* out) {
  h2_pal_result_t result = modem_operation_lock();
  if (result != H2_PAL_OK) return result;
  return modem_operation_unlock(modem_get_status_adapter_impl(user, out));
}

static h2_pal_result_t modem_get_operator_adapter(
    void* user, h2_pal_modem_operator_t* out) {
  h2_pal_result_t result = modem_operation_lock();
  if (result != H2_PAL_OK) return result;
  return modem_operation_unlock(modem_get_operator_adapter_impl(user, out));
}

static h2_pal_result_t modem_get_signal_adapter(void* user,
                                                h2_pal_modem_signal_t* out) {
  h2_pal_result_t result = modem_operation_lock();
  if (result != H2_PAL_OK) return result;
  return modem_operation_unlock(modem_get_signal_adapter_impl(user, out));
}

h2_pal_modem_t* h2_bk7258_board_modem_quectel(void) {
  if (!s_modem_initialized) {
    h2_quectel_modem_config_t config = {
        .init = modem_transport_init,
        .deinit = modem_transport_deinit,
        .flush = modem_transport_flush,
        .read = modem_transport_read,
        .write = modem_transport_write,
        .sync_api = h2_bk7258_board_sync_api(),
        .allocator = h2_bk7258_board_default_allocator(),
        .system_events = h2_bk7258_board_system_event_api(),
        .capabilities =
            H2_PAL_MODEM_CAPABILITY_DATA | H2_PAL_MODEM_CAPABILITY_CALL,
        .command_timeout_ms = 5000u,
        .io_timeout_ms = H2_ZERO_BK_MODEM_AT_READ_MS,
        .cell_locate_token = H2_ZERO_BK_MODEM_CELL_LOCATE_TOKEN,
    };
    h2_pal_result_t result;

    if (rtos_init_semaphore(&s_ppp_event, 1) != kNoErr) {
      return NULL;
    }
    result = h2_quectel_modem_init(&s_modem, &config);
    if (result != H2_PAL_OK) {
      (void)rtos_deinit_semaphore(&s_ppp_event);
      s_ppp_event = NULL;
      return NULL;
    }
    s_quectel_vtable = s_modem.platform.vtable;
    s_modem_vtable = *s_quectel_vtable;
    s_modem_vtable.open = modem_open_adapter;
    s_modem_vtable.close = modem_close_adapter;
    s_modem_vtable.data_open = modem_data_open_adapter;
    s_modem_vtable.data_close = modem_data_close_adapter;
    s_modem_vtable.get_status = modem_get_status_adapter;
    s_modem_vtable.get_identity = modem_get_identity_adapter;
    s_modem_vtable.get_operator = modem_get_operator_adapter;
    s_modem_vtable.get_signal = modem_get_signal_adapter;
    s_modem.platform.vtable = &s_modem_vtable;
    s_modem_initialized = true;
  }
  return h2_quectel_modem_platform(&s_modem);
}
