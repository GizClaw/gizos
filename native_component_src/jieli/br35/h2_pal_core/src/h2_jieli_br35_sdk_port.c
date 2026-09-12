/**
 * @file h2_jieli_br35_sdk_port.c
 * @brief JieLi AC707N (br35) SDK binding for the PAL core providers.
 *
 * This translation unit is the only place in the component that includes SDK
 * headers. It is compiled by the SDK native build (jieli_firmware) with the
 * SDK include roots; host tests link the fake in tests/ instead.
 */

#include "h2_jieli_br35_sdk_port.h"

#include "system/includes.h"
#include "timer.h"

#include <string.h>

#ifndef H2_JIELI_BR35_TASK_PRIORITY
/* BR35 os_api task priority for this initial core provider. */
#define H2_JIELI_BR35_TASK_PRIORITY 2u
#endif

#ifndef H2_JIELI_BR35_TICK_MS
/* BR35 os_type.h: OS_TICKS_PER_SEC = 100. */
#define H2_JIELI_BR35_TICK_MS 10u
#endif

struct h2_jieli_sdk_mutex {
  OS_SEM native;
};

struct h2_jieli_sdk_sem {
  OS_SEM native;
};

/* ---- Memory --------------------------------------------------------------
 * The SDK heap exports malloc/free but no realloc, so every block carries a
 * small aligned header recording its size for realloc copies. */

#define H2_JIELI_BR35_MEM_HEADER 16u

static size_t *mem_header(void *ptr) {
  return (size_t *)((uint8_t *)ptr - H2_JIELI_BR35_MEM_HEADER);
}

void *h2_jieli_sdk_malloc(size_t size) {
  uint8_t *block;
  if (size == 0u || size > SIZE_MAX - H2_JIELI_BR35_MEM_HEADER) {
    return NULL;
  }
  block = (uint8_t *)malloc(size + H2_JIELI_BR35_MEM_HEADER);
  if (block == NULL) {
    return NULL;
  }
  *(size_t *)block = size;
  return block + H2_JIELI_BR35_MEM_HEADER;
}

void *h2_jieli_sdk_realloc(void *ptr, size_t size) {
  void *replacement;
  size_t previous;
  if (ptr == NULL) {
    return h2_jieli_sdk_malloc(size);
  }
  if (size == 0u) {
    h2_jieli_sdk_free(ptr);
    return NULL;
  }
  previous = *mem_header(ptr);
  if (previous >= size) {
    return ptr;
  }
  replacement = h2_jieli_sdk_malloc(size);
  if (replacement == NULL) {
    return NULL;
  }
  memcpy(replacement, ptr, previous);
  h2_jieli_sdk_free(ptr);
  return replacement;
}

void h2_jieli_sdk_free(void *ptr) {
  if (ptr != NULL) {
    free(mem_header(ptr));
  }
}

/* ---- Debug UART ---------------------------------------------------------- */

void h2_jieli_sdk_debug_write(const char *data, size_t length) {
  if (data == NULL || length == 0u) {
    return;
  }
  put_buf((const u8 *)data, (int)length);
}

/* ---- Time ---------------------------------------------------------------- */

void h2_jieli_sdk_critical_enter(void) { local_irq_disable(); }
void h2_jieli_sdk_critical_exit(void) { local_irq_enable(); }

uint32_t h2_jieli_sdk_time_ms(void) { return (uint32_t)jiffies_msec(); }

uint32_t h2_jieli_sdk_tick_ms(void) { return H2_JIELI_BR35_TICK_MS; }

static int ms_to_ticks(uint32_t ms) {
  uint32_t ticks;
  if (ms == 0u) {
    return 0;
  }
  ticks = ms / H2_JIELI_BR35_TICK_MS + (ms % H2_JIELI_BR35_TICK_MS != 0u);
  if (ticks == 0u) {
    ticks = 1u;
  }
  if (ticks > 0x7fffffffu) {
    ticks = 0x7fffffffu;
  }
  return (int)ticks;
}

void h2_jieli_sdk_sleep_ms(uint32_t ms) {
  /* PAL owns the task queue. Pump SDK callbacks while sleeping so sys_timer
   * notifications are dispatched on their registering task. */
  u32 start = jiffies_msec();
  u32 duration = ms ? ms : H2_JIELI_BR35_TICK_MS;
  int msg[16];
  do {
    u32 elapsed = jiffies_msec() - start;
    if (elapsed >= duration)
      break;
    os_taskq_pend_timeout(NULL, msg, 16, ms_to_ticks(duration - elapsed));
  } while (1);
}

/* BR35 uses UCOS os_api storage. Zero timeouts use an interrupt-protected query
 * and consume; pend(0) is reserved for infinite waits. */

static int try_take(void *storage) {
  /* Only CPU0 is started by this board. Excluding its scheduler makes the
   * query-and-consume atomic; pend(0) is used only when a token exists. */
  local_irq_disable();
  int rc = os_sem_query((OS_SEM *)storage) > 0
               ? os_sem_pend((OS_SEM *)storage, 0)
               : OS_TIMEOUT;
  local_irq_enable();
  return rc == OS_NO_ERR ? 0 : (rc == OS_TIMEOUT ? 1 : -1);
}

static int map_os_wait(int rc) {
  if (rc == OS_NO_ERR) {
    return 0;
  }
  return rc == OS_TIMEOUT ? 1 : -1;
}

h2_jieli_sdk_mutex_t *h2_jieli_sdk_mutex_create(void) {
  h2_jieli_sdk_mutex_t *mutex = (h2_jieli_sdk_mutex_t *)malloc(sizeof(*mutex));
  if (mutex == NULL) {
    return NULL;
  }
  if (os_sem_create(&mutex->native, 1) != OS_NO_ERR) {
    free(mutex);
    return NULL;
  }
  return mutex;
}

void h2_jieli_sdk_mutex_destroy(h2_jieli_sdk_mutex_t *mutex) {
  if (mutex == NULL) {
    return;
  }
  (void)os_sem_del(&mutex->native, 0);
  free(mutex);
}

int h2_jieli_sdk_mutex_lock(h2_jieli_sdk_mutex_t *mutex, uint32_t timeout_ms) {
  if (mutex == NULL) {
    return -1;
  }
  if (timeout_ms == 0u) {
    return try_take(&mutex->native);
  }
  if (timeout_ms == H2_JIELI_SDK_WAIT_FOREVER) {
    return map_os_wait(os_sem_pend(&mutex->native, 0));
  }
  return map_os_wait(os_sem_pend(&mutex->native, ms_to_ticks(timeout_ms)));
}

int h2_jieli_sdk_mutex_unlock(h2_jieli_sdk_mutex_t *mutex) {
  if (mutex == NULL) {
    return -1;
  }
  return os_sem_post(&mutex->native) == OS_NO_ERR ? 0 : -1;
}

h2_jieli_sdk_sem_t *h2_jieli_sdk_sem_create(uint32_t initial_count) {
  h2_jieli_sdk_sem_t *sem = (h2_jieli_sdk_sem_t *)malloc(sizeof(*sem));
  if (sem == NULL) {
    return NULL;
  }
  if (os_sem_create(&sem->native, (int)initial_count) != OS_NO_ERR) {
    free(sem);
    return NULL;
  }
  return sem;
}

void h2_jieli_sdk_sem_destroy(h2_jieli_sdk_sem_t *sem) {
  if (sem == NULL) {
    return;
  }
  (void)os_sem_del(&sem->native, 0);
  free(sem);
}

int h2_jieli_sdk_sem_take(h2_jieli_sdk_sem_t *sem, uint32_t timeout_ms) {
  if (sem == NULL) {
    return -1;
  }
  if (timeout_ms == 0u) {
    return try_take(&sem->native);
  }
  if (timeout_ms == H2_JIELI_SDK_WAIT_FOREVER) {
    return map_os_wait(os_sem_pend(&sem->native, 0));
  }
  return map_os_wait(os_sem_pend(&sem->native, ms_to_ticks(timeout_ms)));
}

int h2_jieli_sdk_sem_give(h2_jieli_sdk_sem_t *sem) {
  if (sem == NULL) {
    return -1;
  }
  return os_sem_post(&sem->native) == OS_NO_ERR ? 0 : -1;
}

/* ---- Tasks --------------------------------------------------------------- */

int h2_jieli_sdk_task_create(void (*entry)(void *ctx), void *ctx,
                             const char *name, size_t stack_bytes) {
  u32 stack_words;
  if (entry == NULL || name == NULL) {
    return -1;
  }
  /* os_task_create takes the stack size in 32-bit words. */
  stack_words = (u32)((stack_bytes + 3u) / 4u);
  return os_task_create(entry, ctx, H2_JIELI_BR35_TASK_PRIORITY, stack_words,
                        128, name) == OS_NO_ERR
             ? 0
             : -1;
}

int h2_jieli_sdk_task_delete(const char *name) {
  return os_task_del(name) == OS_NO_ERR ? 0 : -1;
}

void h2_jieli_sdk_task_park(void) {
  for (;;) {
    os_time_dly(100);
  }
}

const void *h2_jieli_sdk_task_current(void) {
  return os_task_get_handle(os_current_task());
}

/* ---- Timers -------------------------------------------------------------- */

uint16_t h2_jieli_sdk_timer_add(void *ctx, void (*callback)(void *ctx),
                                uint32_t period_ms, int repeat) {
  if (callback == NULL || period_ms == 0u) {
    return 0u;
  }
  if (repeat) {
    return sys_timer_add(ctx, callback, period_ms);
  }
  return sys_timeout_add(ctx, callback, period_ms);
}

void h2_jieli_sdk_timer_del(uint16_t id, int repeat) {
  if (id == 0u) {
    return;
  }
  if (repeat) {
    sys_timer_del(id);
  } else {
    sys_timeout_del(id);
  }
}
