#include "h2_jieli_ac791n_devkit_flash_window.h"
#include "h2_jieli_wl82_atomic.h"
#include "asm/sfc_norflash_api.h"
#include "device/ioctl_cmds.h"
#include "os/os_api.h"
#include <stdint.h>

static OS_MUTEX window_mutex;
static volatile uint32_t window_ready;
static uint32_t window_owners, window_saved;
static int window_poisoned;

static int window_lock(void) {
  while (h2_jieli_atomic_load_u32(&window_ready) != 2u) {
    uint32_t expected = 0u;
    if (h2_jieli_atomic_cas_u32(&window_ready, &expected, 1u)) {
      int rc = os_mutex_create(&window_mutex);
      h2_jieli_atomic_store_u32(&window_ready, rc == OS_NO_ERR ? 2u : 0u);
      if (rc != OS_NO_ERR) return -1;
    } else {
      os_time_dly(1u);
    }
  }
  return os_mutex_pend(&window_mutex, 0u) == OS_NO_ERR ? 0 : -1;
}

int h2_jieli_flash_window_open(h2_jieli_flash_window_t *window) {
  if (window == NULL || window_lock() != 0) return -1;
  int rc = -1;
  if (window->active || window_poisoned || window_owners == UINT32_MAX)
    goto done;
  if (window_owners == 0u) {
    /* The SDK ioctl ABI passes a 32-bit target address for GET. */
    if (norflash_ioctl(NULL, IOCTL_GET_WRITE_PROTECT_VALUE,
                       (uint32_t)(uintptr_t)&window_saved) != 0) goto done;
    if (norflash_ioctl(NULL, IOCTL_SET_WRITE_PROTECT, 0u) != 0) {
      /* SET can fail after changing hardware: restore even on open failure. */
      if (norflash_ioctl(NULL, IOCTL_SET_WRITE_PROTECT, window_saved) != 0)
        window_poisoned = 1;
      goto done;
    }
  }
  ++window_owners;
  window->active = 1;
  rc = 0;
done:
  (void)os_mutex_post(&window_mutex);
  return rc;
}

int h2_jieli_flash_window_close(h2_jieli_flash_window_t *window) {
  if (window == NULL || window_lock() != 0) return -1;
  int rc = 0;
  if (window->active) {
    window->active = 0;
    if (--window_owners == 0u &&
        norflash_ioctl(NULL, IOCTL_SET_WRITE_PROTECT, window_saved) != 0) {
      window_poisoned = 1;
      rc = -1;
    }
  }
  (void)os_mutex_post(&window_mutex);
  return rc;
}
