#include "h2_bk7258_board_private.h"

#include "h2_atomic.h"
#include "h2_bk_platform_core.h"
#include "h2_lierda_modem.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "gpio_driver.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "os/os.h"

#include <string.h>

#define AT_COMMAND_MAX 128u
#define AT_LINE_MAX 160u

static h2_pal_result_t transport_flush(void* user);

/* Lierda NT26K-CN B (data-only) on UART2. The public provider owns the AT
 * protocol; this file owns power, UART framing and the host PPP session. */
static struct {
  h2_lierda_modem_t* lierda;
  h2_lierda_command_error_t command_error;
  h2_pal_modem_data_status_t data;
  struct netif netif;
  ppp_pcb* ppp;
  h2_pal_mutex_t* state_lock;
  h2_pal_semaphore_t* link_event;
  h2_pal_semaphore_t* rx_done;
  h2_pal_task_t* rx_task;
  h2_atomic_bool_t rx_stop;
  h2_atomic_int_t rx_error;
  bool atomics_ready;
  bool ppp_started;
  bool close_requested;
  h2_atomic_bool_t ppp_closed;
  bool uart_open;
  bool powered;
} s_modem;

static const h2_pal_sync_api_t* sync_api(void) {
  return h2_bk_platform_sync_api();
}

static uint64_t now_ms(void) {
  uint64_t now = 0;
  (void)h2_pal_time_get_monotonic_ms(h2_bk_platform_time_api(), &now);
  return now;
}

static int output(unsigned gpio, bool high) {
  gpio_config_t config = {.io_mode = GPIO_OUTPUT_ENABLE,
                          .pull_mode = GPIO_PULL_DISABLE,
                          .func_mode = GPIO_SECOND_FUNC_DISABLE};
  if (gpio_dev_unmap((gpio_id_t)gpio) != 0 ||
      bk_gpio_set_config((gpio_id_t)gpio, &config) != 0)
    return H2_PAL_ERR_IO;
  return (high ? bk_gpio_set_output_high((gpio_id_t)gpio)
               : bk_gpio_set_output_low((gpio_id_t)gpio)) == 0
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
}

static h2_pal_result_t transport_init(void* user) {
  (void)user;
  if (s_modem.uart_open) return H2_PAL_OK;
  s_modem.powered = true; /* Even partial rail setup must be unwound. */
  if (output(H2_H202_MODEM_DTR_GPIO, false) != 0 ||
      output(H2_H202_MODEM_ENABLE_GPIO, true) != 0 ||
      output(H2_H202_MODEM_VBUS_ENABLE_GPIO, true) != 0)
    return H2_PAL_ERR_IO;
  s_modem.powered = true;
  rtos_delay_milliseconds(5000);
  const uart_config_t config = {.baud_rate = 115200,
                                .data_bits = UART_DATA_8_BITS,
                                .parity = UART_PARITY_NONE,
                                .stop_bits = UART_STOP_BITS_1,
                                .flow_ctrl = UART_FLOWCTRL_DISABLE,
                                .src_clk = UART_SCLK_XTAL_26M};
  uint32_t charging = bk_gpio_get_value(H2_H202_CHARGING_GPIO);
  uint32_t done = bk_gpio_get_value(H2_H202_CHARGE_DONE_GPIO);
  int rc = bk_uart_init(H2_H202_MODEM_UART_ID, &config);
  s_modem.uart_open = true; /* An unsuccessful native init can own resources. */
  if (gpio_dev_unmap(H2_H202_CHARGING_GPIO) != 0 ||
      bk_gpio_set_value(H2_H202_CHARGING_GPIO, charging) != 0 ||
      gpio_dev_unmap(H2_H202_CHARGE_DONE_GPIO) != 0 ||
      bk_gpio_set_value(H2_H202_CHARGE_DONE_GPIO, done) != 0 ||
      gpio_dev_unmap(H2_H202_MODEM_RX_GPIO) != 0 ||
      gpio_dev_unmap(H2_H202_MODEM_TX_GPIO) != 0 ||
      gpio_dev_map(H2_H202_MODEM_RX_GPIO, GPIO_DEV_UART2_RXD) != 0 ||
      gpio_dev_map(H2_H202_MODEM_TX_GPIO, GPIO_DEV_UART2_TXD) != 0)
    rc = -1;
  /* Native init clears interrupt enables. read_bytes consumes the software
   * FIFO filled by the default RX ISR, so enable it after init and remap. */
  if (rc == 0 && bk_uart_enable_rx_interrupt(H2_H202_MODEM_UART_ID) != 0)
    rc = -1;
  if (rc != 0) return H2_PAL_ERR_IO;
  /* Default UART2 pins carry charger levels before remap. Let the RX ISR
   * drain that hardware FIFO, then discard stale bytes before the first AT. */
  rtos_delay_milliseconds(20);
  return transport_flush(NULL);
}

static h2_pal_result_t transport_deinit(void* user) {
  (void)user;
  if (s_modem.uart_open) {
    if (bk_uart_deinit(H2_H202_MODEM_UART_ID) != 0) return H2_PAL_ERR_IO;
    s_modem.uart_open = false;
  }
  int rc = output(H2_H202_MODEM_VBUS_ENABLE_GPIO, false);
  if (rc == 0) rc = output(H2_H202_MODEM_ENABLE_GPIO, false);
  if (rc == 0) s_modem.powered = false;
  return rc;
}

static h2_pal_result_t transport_read(void* user, uint8_t* data, size_t size,
                                      uint32_t timeout, size_t* count) {
  (void)user;
  *count = 0;
  if (!s_modem.uart_open) return H2_PAL_ERR_CLOSED;
  if (size > UINT32_MAX || timeout == UINT32_MAX) return H2_PAL_ERR_INVALID_ARG;
  int rc =
      bk_uart_read_bytes(H2_H202_MODEM_UART_ID, data, (uint32_t)size, timeout);
  if (rc == BK_ERR_UART_RX_TIMEOUT) return H2_PAL_ERR_TIMEOUT;
  if (rc < 0) return H2_PAL_ERR_IO;
  *count = (size_t)rc;
  return H2_PAL_OK;
}

static h2_pal_result_t transport_write(void* user, const uint8_t* data,
                                       size_t size, uint32_t timeout,
                                       size_t* count) {
  (void)user;
  (void)timeout;
  *count = 0;
  if (!s_modem.uart_open) return H2_PAL_ERR_CLOSED;
  if (size > 2048) return H2_PAL_ERR_INVALID_ARG;
  if (bk_uart_write_bytes(H2_H202_MODEM_UART_ID, data, (uint32_t)size) != 0)
    return H2_PAL_ERR_IO;
  *count = size;
  return H2_PAL_OK;
}

static h2_pal_result_t transport_flush(void* user) {
  (void)user;
  uint8_t discard[128];
  for (unsigned i = 0; i < 32; ++i) {
    int rc =
        bk_uart_read_bytes(H2_H202_MODEM_UART_ID, discard, sizeof(discard), 0);
    if (rc == 0 || rc == BK_ERR_UART_RX_TIMEOUT) return H2_PAL_OK;
    if (rc < 0) return H2_PAL_ERR_IO;
  }
  return H2_PAL_ERR_BUSY;
}

static bool has_prefix(const char* line, const char* prefix) {
  return strncmp(line, prefix, strlen(prefix)) == 0;
}

/* +CME ERROR as a TS 27.007 number, or the exact verbose text the provider
 * classifies under AT+CMEE=2. Anything else stays an unclassified error. */
static bool cme_code(const char* line, uint32_t* out_code) {
  static const struct {
    const char* text;
    uint32_t code;
  } errors[] = {
      {"PH-SIM PIN required", 5},
      {"PH-FSIM PIN required", 6},
      {"PH-FSIM PUK required", 7},
      {"SIM not inserted", 10},
      {"SIM PIN required", 11},
      {"SIM PUK required", 12},
      {"SIM failure", 13},
      {"SIM busy", 14},
      {"SIM wrong", 15},
      {"SIM PIN2 required", 17},
      {"SIM PUK2 required", 18},
      {"network personalization PIN required", 40},
      {"network personalization PUK required", 41},
      {"network subset personalization PIN required", 42},
      {"network subset personalization PUK required", 43},
      {"service provider personalization PIN required", 44},
      {"service provider personalization PUK required", 45},
      {"corporate personalization PIN required", 46},
      {"corporate personalization PUK required", 47},
  };
  const char* p = line + strlen("+CME ERROR:");
  while (*p == ' ') ++p;
  if (*p >= '0' && *p <= '9') {
    uint32_t value = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
      value = value * 10u + (uint32_t)(*p - '0');
      if (value > 65535u) return false;
    }
    if (*p != '\0') return false;
    *out_code = value;
    return true;
  }
  for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
    if (strcmp(p, errors[i].text) == 0) {
      *out_code = errors[i].code;
      return true;
    }
  return false;
}

/* Sends one AT line and collects information lines until the final result.
 * With dial set, a CONNECT line is the successful final result. Stale input
 * (late results, boot URCs) is flushed first; command echo is dropped. */
static h2_pal_result_t exchange(const char* command, char* response,
                                size_t size, uint32_t timeout, bool dial) {
  memset(&s_modem.command_error, 0, sizeof(s_modem.command_error));
  if (command == NULL || response == NULL || size < 2) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  response[0] = '\0';
  if (!s_modem.uart_open) return H2_PAL_ERR_CLOSED;
  if (s_modem.ppp != NULL) return H2_PAL_ERR_BUSY;
  size_t length = 0;
  for (; length < AT_COMMAND_MAX && command[length] != '\0'; ++length)
    if (command[length] < 32 || command[length] > 126)
      return H2_PAL_ERR_INVALID_ARG;
  if (length == 0 || length == AT_COMMAND_MAX) return H2_PAL_ERR_INVALID_ARG;
  char wire[AT_COMMAND_MAX + 1];
  memcpy(wire, command, length);
  wire[length] = '\r';
  size_t written = 0;
  int rc = transport_flush(NULL);
  if (rc == 0)
    rc = transport_write(NULL, (const uint8_t*)wire, length + 1, timeout,
                         &written);
  if (rc != 0) return rc;
  const uint64_t start = now_ms();
  h2_pal_result_t result = H2_PAL_OK;
  char line[AT_LINE_MAX];
  size_t line_size = 0, used = 0;
  for (;;) {
    const uint64_t elapsed = now_ms() - start;
    if (elapsed >= timeout) {
      response[0] = '\0';
      return H2_PAL_ERR_TIMEOUT;
    }
    uint8_t ch = 0;
    size_t got = 0;
    rc = transport_read(NULL, &ch, 1, (uint32_t)(timeout - elapsed), &got);
    if (rc == H2_PAL_ERR_TIMEOUT || (rc == 0 && got == 0)) continue;
    if (rc != 0) {
      response[0] = '\0';
      return rc;
    }
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (ch == '\0') result = H2_PAL_ERR_FORMAT;
      if (line_size + 1 < sizeof(line))
        line[line_size++] = (char)ch;
      else if (result == H2_PAL_OK)
        result = H2_PAL_ERR_TRUNCATED;
      continue;
    }
    line[line_size] = '\0';
    const size_t n = line_size;
    line_size = 0;
    if (n == 0 || strcmp(line, command) == 0) continue;
    if (strcmp(line, "OK") == 0 || (dial && has_prefix(line, "CONNECT"))) {
      if (result != H2_PAL_OK) response[0] = '\0';
      return result;
    }
    if (strcmp(line, "ERROR") == 0 || strcmp(line, "NO CARRIER") == 0 ||
        has_prefix(line, "+CMS ERROR:") || has_prefix(line, "+CME ERROR:")) {
      if (has_prefix(line, "+CME ERROR:") &&
          cme_code(line, &s_modem.command_error.cme_code))
        s_modem.command_error.cme_valid = 1;
      response[0] = '\0';
      return result != H2_PAL_OK ? result : H2_PAL_ERR_IO;
    }
    if (used + n + 2 > size) {
      if (result == H2_PAL_OK) result = H2_PAL_ERR_TRUNCATED;
      continue;
    }
    memcpy(response + used, line, n);
    used += n;
    response[used++] = '\n';
    response[used] = '\0';
  }
}

static h2_pal_result_t at_command(void* user, const char* command,
                                  char* response, size_t response_size,
                                  uint32_t timeout_ms) {
  (void)user;
  return exchange(command, response, response_size, timeout_ms, false);
}

static h2_pal_result_t command_error(void* user,
                                     h2_lierda_command_error_t* out_error) {
  (void)user;
  if (out_error == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_error = s_modem.command_error;
  return H2_PAL_OK;
}

static void link_status(ppp_pcb* ppp, int error, void* user) {
  (void)ppp;
  (void)user;
  if (h2_pal_mutex_lock(sync_api(), s_modem.state_lock) != 0) {
    h2_atomic_int_store(&s_modem.rx_error, H2_PAL_ERR_IO, H2_ATOMIC_RELEASE);
    return;
  }
  if (error == PPPERR_NONE) {
    s_modem.data.state = H2_PAL_MODEM_DATA_OPEN;
    s_modem.data.ip4 = ip4_addr_get_u32(netif_ip4_addr(&s_modem.netif));
    s_modem.data.ip4_valid = s_modem.data.ip4 != 0;
    s_modem.data.dns1_ip4 = ip4_addr_get_u32(ip_2_ip4(dns_getserver(0)));
    s_modem.data.last_error =
        s_modem.data.ip4_valid ? H2_PAL_OK : H2_PAL_ERR_IO;
  } else {
    h2_atomic_bool_store(&s_modem.ppp_closed, true, H2_ATOMIC_RELEASE);
    s_modem.data.state = H2_PAL_MODEM_DATA_CLOSED;
    s_modem.data.ip4_valid = 0;
    s_modem.data.last_error =
        error == PPPERR_USER ? H2_PAL_OK : H2_PAL_ERR_UNAVAILABLE;
  }
  if (h2_pal_mutex_unlock(sync_api(), s_modem.state_lock) != 0)
    h2_atomic_int_store(&s_modem.rx_error, H2_PAL_ERR_IO, H2_ATOMIC_RELEASE);
  (void)h2_pal_semaphore_give(sync_api(), s_modem.link_event);
}

static u32_t ppp_output(ppp_pcb* ppp, u8_t* data, u32_t size, void* user) {
  (void)ppp;
  (void)user;
  size_t written = 0;
  int rc = transport_write(NULL, data, size, 100, &written);
  if (rc != 0) h2_atomic_int_store(&s_modem.rx_error, rc, H2_ATOMIC_RELEASE);
  return rc == 0 ? (u32_t)written : 0;
}

static void receive_ppp(void* user) {
  (void)user;
  uint8_t data[512];
  while (!h2_atomic_bool_load(&s_modem.rx_stop, H2_ATOMIC_ACQUIRE)) {
    size_t size = 0;
    int rc = transport_read(NULL, data, sizeof(data), 50, &size);
    if (rc == H2_PAL_ERR_TIMEOUT) continue;
    if (rc != 0 || (size != 0 && pppos_input_tcpip(s_modem.ppp, data,
                                                   (int)size) != ERR_OK)) {
      h2_atomic_int_store(&s_modem.rx_error, H2_PAL_ERR_IO, H2_ATOMIC_RELEASE);
      break;
    }
  }
  (void)h2_pal_semaphore_give(sync_api(), s_modem.rx_done);
}

static h2_pal_result_t data_status(void* user,
                                   h2_pal_modem_data_status_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  int rc = h2_pal_mutex_lock(sync_api(), s_modem.state_lock);
  if (rc != 0) return rc;
  *out = s_modem.data;
  rc = h2_pal_mutex_unlock(sync_api(), s_modem.state_lock);
  int io = h2_atomic_int_load(&s_modem.rx_error, H2_ATOMIC_ACQUIRE);
  if (io != 0) {
    out->ip4_valid = 0;
    out->last_error = io;
    return io;
  }
  return rc;
}

/* Ends the host PPP session; the module stays powered in command mode. */
static h2_pal_result_t data_close(void* user, uint32_t timeout) {
  (void)user;
  if (timeout == 0 || timeout > 5000) timeout = 5000;
  if (s_modem.ppp != NULL && s_modem.ppp_started &&
      !h2_atomic_bool_load(&s_modem.ppp_closed, H2_ATOMIC_ACQUIRE)) {
    if (!s_modem.close_requested) {
      if (pppapi_close(s_modem.ppp, 0) != ERR_OK) return H2_PAL_ERR_IO;
      s_modem.close_requested = true;
    }
    const uint64_t start = now_ms();
    while (!h2_atomic_bool_load(&s_modem.ppp_closed, H2_ATOMIC_ACQUIRE)) {
      int rc = h2_pal_semaphore_take(sync_api(), s_modem.link_event, timeout);
      if (rc != 0) return rc;
      if (now_ms() - start >= timeout) return H2_PAL_ERR_TIMEOUT;
    }
  }
  if (s_modem.atomics_ready)
    h2_atomic_bool_store(&s_modem.rx_stop, true, H2_ATOMIC_RELEASE);
  if (s_modem.rx_task != NULL) {
    int rc = h2_pal_semaphore_take(sync_api(), s_modem.rx_done, timeout);
    if (rc != 0) return rc;
    rc = h2_pal_task_join(h2_bk_platform_task_api(), s_modem.rx_task);
    if (rc != 0) {
      (void)h2_pal_semaphore_give(sync_api(), s_modem.rx_done);
      return rc;
    }
    s_modem.rx_task = NULL;
  }
  if (s_modem.ppp != NULL) {
    if (pppapi_free(s_modem.ppp) != ERR_OK) return H2_PAL_ERR_IO;
    s_modem.ppp = NULL;
    s_modem.ppp_started = false;
    s_modem.close_requested = false;
    h2_bk_platform_netif_unregister(&s_modem.netif);
  }
  if (s_modem.atomics_ready)
    h2_atomic_int_store(&s_modem.rx_error, 0, H2_ATOMIC_RELEASE);
  if (s_modem.state_lock != NULL) {
    int rc = h2_pal_mutex_lock(sync_api(), s_modem.state_lock);
    if (rc != 0) return rc;
    memset(&s_modem.data, 0, sizeof(s_modem.data));
    s_modem.data.state = H2_PAL_MODEM_DATA_CLOSED;
    rc = h2_pal_mutex_unlock(sync_api(), s_modem.state_lock);
    if (rc != 0) return rc;
  }
  return H2_PAL_OK;
}

/* Powers the module and waits until it answers AT. */
static h2_pal_result_t transport_open(void* user, uint32_t timeout) {
  (void)user;
  if (timeout == 0 || timeout > 60000) timeout = 60000;
  int rc = transport_init(NULL);
  if (rc != 0) return rc;
  char response[64];
  const uint64_t start = now_ms();
  do {
    rc = exchange("AT", response, sizeof(response), 1000, false);
    if (rc == 0) return H2_PAL_OK;
    if (rc != H2_PAL_ERR_TIMEOUT) rtos_delay_milliseconds(200);
  } while (now_ms() - start < timeout);
  return rc;
}

static h2_pal_result_t transport_close(void* user, uint32_t timeout) {
  int rc = data_close(user, timeout);
  return rc == 0 ? transport_deinit(NULL) : rc;
}

/* The provider has already written the APN to context 1 with AT+CGDCONT. */
static h2_pal_result_t data_open(void* user,
                                 const h2_pal_modem_apn_config_t* apn,
                                 uint32_t timeout) {
  (void)user;
  if (apn == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (apn->username[0] != '\0' || apn->password[0] != '\0')
    return H2_PAL_ERR_UNSUPPORTED; /* No PPP authentication on this board. */
  if (s_modem.ppp != NULL || s_modem.rx_task != NULL) return H2_PAL_ERR_BUSY;
  if (timeout == 0 || timeout > 60000) timeout = 60000;
  char response[64];
  int rc = exchange("ATD*99***1#", response, sizeof(response),
                    timeout < 20000 ? timeout : 20000, true);
  if (rc != 0) return rc;
  h2_atomic_int_store(&s_modem.rx_error, 0, H2_ATOMIC_RELEASE);
  h2_atomic_bool_store(&s_modem.rx_stop, false, H2_ATOMIC_RELEASE);
  (void)h2_pal_semaphore_take(sync_api(), s_modem.link_event, 0);
  (void)h2_pal_semaphore_take(sync_api(), s_modem.rx_done, 0);
  memset(&s_modem.netif, 0, sizeof(s_modem.netif));
  s_modem.ppp =
      pppapi_pppos_create(&s_modem.netif, ppp_output, link_status, NULL);
  if (s_modem.ppp == NULL) return H2_PAL_ERR_NO_MEMORY;
  rc = h2_pal_mutex_lock(sync_api(), s_modem.state_lock);
  if (rc != 0) return rc;
  s_modem.data =
      (h2_pal_modem_data_status_t){.state = H2_PAL_MODEM_DATA_OPENING};
  rc = h2_pal_mutex_unlock(sync_api(), s_modem.state_lock);
  if (rc != 0) return rc;
  const h2_pal_task_options_t task = {.name = "h202/ppp-rx",
                                      .min_stack_size = 8192};
  rc = h2_pal_task_start(h2_bk_platform_task_api(), &task, receive_ppp, NULL,
                         &s_modem.rx_task);
  if (rc != 0) return rc;
  h2_atomic_bool_store(&s_modem.ppp_closed, false, H2_ATOMIC_RELEASE);
  if (pppapi_connect(s_modem.ppp, 0) != ERR_OK) {
    h2_atomic_bool_store(&s_modem.ppp_closed, true, H2_ATOMIC_RELEASE);
    return H2_PAL_ERR_IO;
  }
  s_modem.ppp_started = true;
  rc = h2_pal_semaphore_take(sync_api(), s_modem.link_event, timeout);
  if (rc != 0) return rc;
  h2_pal_modem_data_status_t status;
  rc = data_status(NULL, &status);
  if (rc != 0) return rc;
  if (status.state != H2_PAL_MODEM_DATA_OPEN || !status.ip4_valid)
    return H2_PAL_ERR_UNAVAILABLE;
  h2_bk_platform_netif_register(&s_modem.netif, H2_PAL_NETIF_KIND_MODEM_DATA);
  return H2_PAL_OK;
}

h2_pal_result_t h2_h202_modem_init(void) {
  if (s_modem.lierda != NULL) return H2_PAL_OK;
  if (s_modem.state_lock != NULL || s_modem.rx_stop.storage != NULL ||
      s_modem.rx_error.storage != NULL || s_modem.ppp_closed.storage != NULL) {
    int rc = h2_h202_modem_deinit();
    if (rc != 0) return rc;
  }
  if (h2_atomic_bool_init(&s_modem.rx_stop, false) != H2_ATOMIC_OK ||
      h2_atomic_int_init(&s_modem.rx_error, 0) != H2_ATOMIC_OK ||
      h2_atomic_bool_init(&s_modem.ppp_closed, true) != H2_ATOMIC_OK)
    return H2_PAL_ERR_NO_MEMORY;
  s_modem.atomics_ready = true;
  s_modem.data.state = H2_PAL_MODEM_DATA_CLOSED;
  const h2_pal_mutex_config_t mutex = {
      .name = "h202/ppp-state",
      .allocator = h2_bk_platform_default_allocator()};
  int rc = h2_pal_mutex_create(sync_api(), &mutex, &s_modem.state_lock);
  if (rc != 0) return rc;
  const h2_pal_semaphore_config_t semaphore = {
      .name = "h202/ppp-event",
      .max_count = 1,
      .initial_count = 0,
      .allocator = h2_bk_platform_default_allocator()};
  rc = h2_pal_semaphore_create(sync_api(), &semaphore, &s_modem.link_event);
  if (rc == 0)
    rc = h2_pal_semaphore_create(sync_api(), &semaphore, &s_modem.rx_done);
  if (rc != 0) return rc;
  /* The provider needs a valid APN at create; MFG sets the real one from the
   * IMSI before data_open. */
  const h2_lierda_modem_config_t config = {
      .model = H2_LIERDA_MODEM_MODEL_NT26KCNB20NNC,
      .transport = {.open = transport_open,
                    .close = transport_close,
                    .command = at_command,
                    .data_open = data_open,
                    .data_close = data_close,
                    .data_status = data_status,
                    .get_command_error = command_error},
      .allocator = h2_bk_platform_default_allocator(),
      .sync_api = sync_api(),
      .apn = {.apn = "cmnet"},
      .command_timeout_ms = 5000,
  };
  return h2_lierda_modem_create(&config, &s_modem.lierda);
}

const h2_pal_modem_api_t* h2_h202_modem_api(void) {
  return s_modem.lierda != NULL ? h2_lierda_modem_api(s_modem.lierda) : NULL;
}

h2_pal_result_t h2_h202_modem_deinit(void) {
  if (s_modem.lierda != NULL) {
    int rc = h2_lierda_modem_destroy(s_modem.lierda);
    if (rc != 0) return rc;
    s_modem.lierda = NULL;
  } else if (s_modem.uart_open || s_modem.powered) {
    int rc = transport_close(NULL, 5000);
    if (rc != 0) return rc;
  }
  h2_atomic_int_destroy(&s_modem.rx_error);
  h2_atomic_bool_destroy(&s_modem.rx_stop);
  h2_atomic_bool_destroy(&s_modem.ppp_closed);
  s_modem.atomics_ready = false;
  if (s_modem.rx_done != NULL) {
    int rc = h2_pal_semaphore_destroy(sync_api(), s_modem.rx_done);
    if (rc != 0) return rc;
    s_modem.rx_done = NULL;
  }
  if (s_modem.link_event != NULL) {
    int rc = h2_pal_semaphore_destroy(sync_api(), s_modem.link_event);
    if (rc != 0) return rc;
    s_modem.link_event = NULL;
  }
  if (s_modem.state_lock != NULL) {
    int rc = h2_pal_mutex_destroy(sync_api(), s_modem.state_lock);
    if (rc != 0) return rc;
    s_modem.state_lock = NULL;
  }
  memset(&s_modem, 0, sizeof(s_modem));
  return H2_PAL_OK;
}
