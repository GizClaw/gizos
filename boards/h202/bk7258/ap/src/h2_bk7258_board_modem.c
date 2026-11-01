#include "h2_bk7258_board_private.h"

#include "h2_atomic.h"
#include "h2_bk_platform_core.h"
#include "h2_quectel_modem.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "gpio_driver.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "os/os.h"

#include <string.h>

static struct {
  h2_quectel_modem_t quectel;
  h2_pal_modem_vtable_t methods;
  const h2_pal_modem_vtable_t* protocol;
  h2_pal_modem_status_t status;
  h2_pal_modem_identity_t identity;
  h2_pal_modem_signal_t signal;
  h2_pal_modem_data_status_t data;
  struct netif netif;
  ppp_pcb* ppp;
  h2_pal_mutex_t* state_lock;
  h2_pal_semaphore_t* link_event;
  h2_pal_semaphore_t* rx_done;
  h2_pal_task_t* rx_task;
  h2_atomic_bool_t rx_stop;
  h2_atomic_int_t rx_error;
  bool initialized;
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
  return rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
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

static h2_pal_result_t close_modem(void* user, uint32_t timeout) {
  (void)user;
  if (timeout == 0 || timeout > 5000) timeout = 5000;
  if (s_modem.ppp != NULL && s_modem.ppp_started &&
      !h2_atomic_bool_load(&s_modem.ppp_closed, H2_ATOMIC_ACQUIRE)) {
    if (!s_modem.close_requested) {
      if (pppapi_close(s_modem.ppp, 0) != ERR_OK) return H2_PAL_ERR_IO;
      s_modem.close_requested = true;
    }
    uint64_t start = 0, now = 0;
    if (h2_pal_time_get_monotonic_ms(h2_bk_platform_time_api(), &start) != 0)
      return H2_PAL_ERR_IO;
    while (!h2_atomic_bool_load(&s_modem.ppp_closed, H2_ATOMIC_ACQUIRE)) {
      int rc = h2_pal_semaphore_take(sync_api(), s_modem.link_event, timeout);
      if (rc != 0) return rc;
      if (h2_pal_time_get_monotonic_ms(h2_bk_platform_time_api(), &now) != 0)
        return H2_PAL_ERR_IO;
      if (now - start >= timeout) return H2_PAL_ERR_TIMEOUT;
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
  int rc = transport_deinit(NULL);
  if (rc != 0) return rc;
  rc = s_modem.initialized ? h2_quectel_modem_transport_closed(&s_modem.quectel)
                           : H2_PAL_OK;
  if (rc != 0) return rc;
  memset(&s_modem.data, 0, sizeof(s_modem.data));
  return H2_PAL_OK;
}

static h2_pal_result_t open_data(void* user, uint32_t timeout) {
  (void)user;
  if (s_modem.ppp != NULL || s_modem.rx_task != NULL) return H2_PAL_ERR_BUSY;
  if (timeout == 0 || timeout > 60000) timeout = 60000;
  int rc = s_modem.protocol->get_status(&s_modem.quectel, &s_modem.status);
  if (rc == 0)
    rc = s_modem.protocol->get_identity(&s_modem.quectel, &s_modem.identity);
  if (rc == 0)
    rc = s_modem.protocol->get_signal(&s_modem.quectel, &s_modem.signal);
  if (rc == 0) rc = h2_quectel_modem_dial_ppp(&s_modem.quectel);
  if (rc != 0) return rc;
  h2_atomic_int_store(&s_modem.rx_error, 0, H2_ATOMIC_RELEASE);
  h2_atomic_bool_store(&s_modem.rx_stop, false, H2_ATOMIC_RELEASE);
  (void)h2_pal_semaphore_take(sync_api(), s_modem.link_event, 0);
  (void)h2_pal_semaphore_take(sync_api(), s_modem.rx_done, 0);
  memset(&s_modem.netif, 0, sizeof(s_modem.netif));
  s_modem.ppp =
      pppapi_pppos_create(&s_modem.netif, ppp_output, link_status, NULL);
  if (s_modem.ppp == NULL) return H2_PAL_ERR_NO_MEMORY;
  s_modem.data =
      (h2_pal_modem_data_status_t){.state = H2_PAL_MODEM_DATA_OPENING};
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

static h2_pal_result_t observed_status(void* user, h2_pal_modem_status_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (s_modem.ppp == NULL) return s_modem.protocol->get_status(user, out);
  *out = s_modem.status;
  h2_pal_modem_data_status_t data;
  int rc = data_status(NULL, &data);
  out->packet = rc == 0 && data.ip4_valid ? H2_PAL_MODEM_PACKET_CONNECTED
                                          : H2_PAL_MODEM_PACKET_DETACHED;
  return rc;
}

static h2_pal_result_t observed_identity(void* user,
                                         h2_pal_modem_identity_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (s_modem.ppp == NULL) return s_modem.protocol->get_identity(user, out);
  *out = s_modem.identity;
  return H2_PAL_OK;
}

static h2_pal_result_t observed_signal(void* user, h2_pal_modem_signal_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (s_modem.ppp == NULL) return s_modem.protocol->get_signal(user, out);
  *out = s_modem.signal;
  return H2_PAL_OK;
}

h2_pal_result_t h2_h202_modem_init(void) {
  if (s_modem.initialized) return H2_PAL_OK;
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
  const h2_quectel_modem_config_t config = {
      .init = transport_init,
      .deinit = transport_deinit,
      .read = transport_read,
      .write = transport_write,
      .flush = transport_flush,
      .sync_api = sync_api(),
      .allocator = h2_bk_platform_default_allocator(),
      .capabilities = H2_PAL_MODEM_CAPABILITY_DATA,
      .command_timeout_ms = 5000,
      .io_timeout_ms = 8000,
  };
  rc = h2_quectel_modem_init(&s_modem.quectel, &config);
  if (rc != 0) return rc;
  s_modem.protocol = s_modem.quectel.platform.vtable;
  s_modem.methods = *s_modem.protocol;
  s_modem.methods.close = close_modem;
  s_modem.methods.data_open = open_data;
  s_modem.methods.data_close = close_modem;
  s_modem.methods.get_data_status = data_status;
  s_modem.methods.get_status = observed_status;
  s_modem.methods.get_identity = observed_identity;
  s_modem.methods.get_signal = observed_signal;
  s_modem.quectel.platform.vtable = &s_modem.methods;
  s_modem.initialized = true;
  return H2_PAL_OK;
}

const h2_pal_modem_api_t* h2_h202_modem_api(void) {
  return s_modem.initialized ? &s_modem.quectel.platform : NULL;
}

h2_pal_result_t h2_h202_modem_deinit(void) {
  if (s_modem.initialized || s_modem.uart_open || s_modem.powered) {
    int rc = close_modem(NULL, 5000);
    if (rc != 0) return rc;
  }
  if (s_modem.initialized) {
    int rc = h2_quectel_modem_deinit(&s_modem.quectel);
    if (rc != 0) return rc;
    s_modem.initialized = false;
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
