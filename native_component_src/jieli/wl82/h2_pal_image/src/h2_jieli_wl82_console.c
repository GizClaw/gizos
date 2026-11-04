#include "h2_jieli_wl82_console.h"
#include "h2_jieli_wl82_atomic.h"
#ifdef H2_JIELI_CONSOLE_HOST_TEST
#include "h2_jieli_console_test_sdk.h"
#else
#include "app_config.h"
#include "asm/includes.h"
#include "os/os_api.h"
#include "usb/device/cdc.h"
#include "usb/usb_common_def.h"
#endif
#include <string.h>

enum {
  H2_USB_PACKET_SIZE = 64,
  H2_USB_RX_RING_SIZE = 8192,
  H2_PHYSICAL_POLL_MS = 10,
  H2_USB_RX_TASK_STACK_SIZE = 4096
};
typedef struct h2_jieli_console {
  volatile usb_dev usb_id;
  OS_SEM rx_wakeup_sem;
  OS_SEM rx_data_sem;
  OS_MUTEX rx_mutex;
  OS_MUTEX tx_mutex;
  uint8_t rx_ring[H2_USB_RX_RING_SIZE];
  size_t rx_head, rx_tail, rx_count;
  uint32_t rx_overflow;
  int initialized, started;
} h2_jieli_console_t;
static h2_jieli_console_t state;
static volatile uint32_t debug_draining;
/* The SDK printf worker also runs in diagnostic images without a command
 * console. Raw batches must keep flowing there, and startup must wait until a
 * raw batch retires before publishing the synchronized physical console. */
enum { DEBUG_RAW_IDLE, DEBUG_RAW_BUSY, DEBUG_STARTING, DEBUG_READY };
static volatile uint32_t debug_gate;

int h2_jieli_usb_debug_try_lock(void) {
  uint32_t expected = DEBUG_RAW_IDLE;
  if (h2_jieli_atomic_cas_u32(&debug_gate, &expected, DEBUG_RAW_BUSY)) {
    h2_jieli_atomic_store_u32(&debug_draining, 1u);
    return 1;
  }
  if (expected != DEBUG_READY || os_mutex_accept(&state.tx_mutex) != OS_NO_ERR)
    return 0;
  h2_jieli_atomic_store_u32(&debug_draining, 1u);
  return 1;
}

void h2_jieli_usb_debug_unlock(void) {
  /* Contenders never change the successful owner's release state. */
  h2_jieli_atomic_store_u32(&debug_draining, 0u);
  if (h2_jieli_atomic_load_u32(&debug_gate) == DEBUG_RAW_BUSY)
    h2_jieli_atomic_store_u32(&debug_gate, DEBUG_RAW_IDLE);
  else
    (void)os_mutex_post(&state.tx_mutex);
}

int h2_jieli_usb_debug_is_draining(void) {
  return h2_jieli_atomic_load_u32(&debug_draining) != 0u;
}
static uint32_t ms_to_ticks(uint32_t ms) {
  uint32_t ticks = ms / 10u + (ms % 10u != 0u);
  return ticks == 0u ? 1u : ticks;
}

#if !defined CONFIG_H2_UART1_DEBUG_ENABLE
static void usb_cdc_wakeup(struct usb_device_t* device) {
  state.usb_id = usb_device2id(device);
  os_sem_post(&state.rx_wakeup_sem);
}

static void usb_rx_task(void* user) {
  h2_jieli_console_t* self = user != NULL ? user : &state;
  uint8_t packet[H2_USB_PACKET_SIZE] __attribute__((aligned(4)));
  for (;;) {
    (void)os_sem_pend(&self->rx_wakeup_sem, 0u);
    for (uint32_t packets = 0u; packets < 32u; ++packets) {
      uint32_t received = cdc_read_data(self->usb_id, packet, sizeof(packet));
      if (received == 0u) break;
      if (os_mutex_pend(&self->rx_mutex, 0u) != OS_NO_ERR) continue;
      for (uint32_t i = 0u; i < received; ++i) {
        if (self->rx_count == sizeof(self->rx_ring)) {
          ++self->rx_overflow;
          break;
        }
        self->rx_ring[self->rx_head] = packet[i];
        self->rx_head = (self->rx_head + 1u) % sizeof(self->rx_ring);
        ++self->rx_count;
      }
      (void)os_mutex_post(&self->rx_mutex);
      os_sem_post(&self->rx_data_sem);
      if (packets == 31u) {
        os_sem_post(&self->rx_wakeup_sem);
        os_time_dly(1u);
      }
    }
  }
}

#endif

h2_pal_result_t h2_jieli_wl82_console_read(void* user, void* buffer, size_t len,
                                           size_t* out_read,
                                           uint32_t timeout_ms) {
  h2_jieli_console_t* self = user != NULL ? user : &state;
  if (self == NULL || buffer == NULL || out_read == NULL || len == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_read = 0u;
  uint32_t started = timer_get_ms();
  for (;;) {
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
    int count = h2_jieli_wl82_console_board_config()->read(buffer, len);
    if (count < 0) return count;
    size_t take = (size_t)count;
#else
    if (os_mutex_pend(&self->rx_mutex, 0u) != OS_NO_ERR) {
      return H2_PAL_ERR_IO;
    }
    size_t take = len < self->rx_count ? len : self->rx_count;
    for (size_t i = 0u; i < take; ++i) {
      ((uint8_t*)buffer)[i] = self->rx_ring[self->rx_tail];
      self->rx_tail = (self->rx_tail + 1u) % sizeof(self->rx_ring);
    }
    self->rx_count -= take;
    (void)os_mutex_post(&self->rx_mutex);
#endif
    if (take != 0u) {
      *out_read = take;
      return H2_PAL_OK;
    }
    uint32_t elapsed = timer_get_ms() - started;
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    uint32_t remaining = timeout_ms - elapsed;
    uint32_t wait =
        remaining < H2_PHYSICAL_POLL_MS ? remaining : H2_PHYSICAL_POLL_MS;
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
    os_time_dly(ms_to_ticks(wait));
#else
    (void)os_sem_pend(&self->rx_data_sem, ms_to_ticks(wait));
#endif
  }
}

h2_pal_result_t h2_jieli_wl82_console_write(void* user, const void* buffer,
                                            size_t len, size_t* out_written,
                                            uint32_t timeout_ms) {
  h2_jieli_console_t* self = user != NULL ? user : &state;
  if (self == NULL || (buffer == NULL && len != 0u) || out_written == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_written = 0u;
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
  int count =
      h2_jieli_wl82_console_board_config()->write(buffer, len, timeout_ms);
  if (count < 0) return count;
  *out_written = (size_t)count;
  return *out_written <= len ? H2_PAL_OK : H2_PAL_ERR_IO;
#else
  const uint8_t* cursor = buffer;
  if (len == 0u) return H2_PAL_OK;
  uint32_t started = timer_get_ms();
  while (os_mutex_accept(&self->tx_mutex) != OS_NO_ERR) {
    if (timeout_ms == 0u) return H2_PAL_ERR_TIMEOUT;
    uint32_t elapsed = timer_get_ms() - started;
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    uint32_t ticks = (timeout_ms - elapsed) / 10u;
    if (ticks != 0u && os_mutex_pend(&self->tx_mutex, (int)ticks) == OS_NO_ERR)
      break;
  }
  int result = H2_PAL_OK;
  while (*out_written < len) {
    uint32_t elapsed = timeout_ms == 0u ? 0u : timer_get_ms() - started;
    if (timeout_ms != 0u && elapsed >= timeout_ms) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    size_t remaining = len - *out_written;
    uint32_t take = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    /* The board CDC patch performs one immediately available short packet;
     * it never waits on native TX or a trailing zero-length packet. */
    uint32_t written = cdc_write_data(
        self->usb_id, (uint8_t*)(uintptr_t)&cursor[*out_written], take);
    if (written > take) {
      result = H2_PAL_ERR_IO;
      break;
    }
    *out_written += written;
    if (written == 0u) {
      if (timeout_ms == 0u) {
        result = H2_PAL_ERR_TIMEOUT;
        break;
      }
      elapsed = timer_get_ms() - started;
      if (elapsed >= timeout_ms) {
        result = H2_PAL_ERR_TIMEOUT;
        break;
      }
      if (timeout_ms - elapsed >= 10u) os_time_dly(1u);
    }
  }
  (void)os_mutex_post(&self->tx_mutex);
  return result;
#endif
}

h2_pal_result_t h2_jieli_wl82_console_flush(void* user) {
  (void)user;
  return H2_PAL_OK;
}

/* No callbacks or tasks may observe these objects until every create succeeds.
 */
static int console_sync_init(h2_jieli_console_t* self) {
  if (os_mutex_create(&self->rx_mutex) != OS_NO_ERR) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  if (os_mutex_create(&self->tx_mutex) != OS_NO_ERR) {
    goto fail_tx;
  }
  if (os_sem_create(&self->rx_wakeup_sem, 0) != OS_NO_ERR) {
    goto fail_wakeup;
  }
  if (os_sem_create(&self->rx_data_sem, 0) != OS_NO_ERR) {
    goto fail_data;
  }
  return H2_PAL_OK;

fail_data:
  (void)os_sem_del(&self->rx_wakeup_sem, 0);
fail_wakeup:
  (void)os_mutex_del(&self->tx_mutex, 0);
fail_tx:
  (void)os_mutex_del(&self->rx_mutex, 0);
  return H2_PAL_ERR_NO_MEMORY;
}

/* Only valid before callbacks/workers have been published. */
static void console_sync_destroy(h2_jieli_console_t* self) {
  (void)os_sem_del(&self->rx_data_sem, 0);
  (void)os_sem_del(&self->rx_wakeup_sem, 0);
  (void)os_mutex_del(&self->tx_mutex, 0);
  (void)os_mutex_del(&self->rx_mutex, 0);
}

int h2_jieli_wl82_console_start(void) {
  if (state.initialized) return H2_PAL_OK;
  for (;;) {
    uint32_t expected = DEBUG_RAW_IDLE;
    if (h2_jieli_atomic_cas_u32(&debug_gate, &expected, DEBUG_STARTING)) break;
    if (expected != DEBUG_RAW_BUSY) return H2_PAL_ERR_BUSY;
    os_time_dly(1u);
  }
  int rc = console_sync_init(&state);
  if (rc != H2_PAL_OK) {
    h2_jieli_atomic_store_u32(&debug_gate, DEBUG_RAW_IDLE);
    return rc;
  }
  const h2_jieli_wl82_console_config_t* config =
      h2_jieli_wl82_console_board_config();
  if (config == NULL || config->start == NULL || config->start() != H2_PAL_OK) {
    console_sync_destroy(&state);
    h2_jieli_atomic_store_u32(&debug_gate, DEBUG_RAW_IDLE);
    return H2_PAL_ERR_IO;
  }
  state.usb_id = 0;
#if !defined CONFIG_H2_UART1_DEBUG_ENABLE
  if (os_task_create(usb_rx_task, &state, 12, H2_USB_RX_TASK_STACK_SIZE, 0,
                     "h2console_rx") != OS_NO_ERR) {
    console_sync_destroy(&state);
    h2_jieli_atomic_store_u32(&debug_gate, DEBUG_RAW_IDLE);
    return H2_PAL_ERR_NO_MEMORY;
  }
  /* No operation can fail after publishing the worker's address-stable state.
   */
  for (usb_dev id = 0; id < USB_MAX_HW_NUM; ++id)
    cdc_set_wakeup_handler(id, usb_cdc_wakeup);
#endif
  state.initialized = 1;
  state.started = 1;
  h2_jieli_atomic_store_u32(&debug_gate, DEBUG_READY);
  return H2_PAL_OK;
}

void h2_jieli_wl82_diag_write(const char* text) {
  if (text == NULL || !state.started) return;
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
  (void)h2_jieli_wl82_console_board_config()->write(text, strlen(text), 100u);
#else
  if (os_mutex_accept(&state.tx_mutex) != OS_NO_ERR) return;
  size_t remaining = strlen(text);
  while (remaining != 0u) {
    uint32_t take = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    uint32_t written =
        cdc_write_data(state.usb_id, (uint8_t*)(uintptr_t)text, take);
    if (written == 0u || written > take) break;
    text += written;
    remaining -= written;
  }
  (void)os_mutex_post(&state.tx_mutex);
#endif
}

void h2_jieli_loader_diag_write(const char* text) {
  h2_jieli_wl82_diag_write(text);
}
