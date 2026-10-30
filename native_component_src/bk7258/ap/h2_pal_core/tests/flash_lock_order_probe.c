#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONFIG_FREERTOS_SMP 1
#define CONFIG_FLASH_CP_AP_DIRECT_ACCESS 1
#define FLASH_SHARED_CPU_CP 0
#define FLASH_SHARED_CPU_AP 1
#define FLASH_SHARED_CPU_APX 2
#define FLASH_SHARED_CPU_MAX 3
#define BK_OK 0
#define BK_FAIL -1
#define BK_ERR_PARAM -2
#define BK_ERR_TIMEOUT -3
#define BK_ASSERT(value) assert(value)
#define BK_LOGE(tag, ...) fprintf(stderr, __VA_ARGS__)
#define BK_LOGI(tag, ...) fprintf(stderr, __VA_ARGS__)
#define SPINLOCK_SECTION
#define SPIN_LOCK_INIT {PTHREAD_MUTEX_INITIALIZER, ATOMIC_VAR_INIT(3)}

typedef int bk_err_t;
typedef pthread_mutex_t* beken_mutex_t;
typedef struct {
  pthread_mutex_t native;
  atomic_uint owner;
} spinlock_t;

static _Thread_local unsigned cpu_id, irq_disabled;
static atomic_uint irq_snapshot[3], suspended[3], waiting_local[3], returned[3];
static atomic_uint writer_ready, allow_writer;
static bool overlap;
static _Alignas(64) unsigned char shared_region[128];
#define PWR_MNG_FLASH_SHARED_ADDR ((uintptr_t)shared_region)
#define PWR_MNG_FLASH_SHARED_SIZE sizeof(shared_region)

static uint32_t rtos_get_core_id(void) { return cpu_id; }
static uint32_t rtos_disable_int(void) {
  unsigned old = irq_disabled;
  irq_disabled = 1;
  atomic_store(&irq_snapshot[cpu_id], 1);
  return old;
}
static void rtos_enable_int(uint32_t flags) {
  irq_disabled = flags;
  atomic_store(&irq_snapshot[cpu_id], flags);
}
static int rtos_is_in_interrupt_context(void) { return 0; }
static int rtos_local_irq_disabled(void) { return irq_disabled; }
static int rtos_lock_mutex(beken_mutex_t* mutex) {
  return pthread_mutex_lock(*mutex);
}
static int rtos_unlock_mutex(beken_mutex_t* mutex) {
  return pthread_mutex_unlock(*mutex);
}
static void vTaskSuspendAll(void) { atomic_fetch_add(&suspended[cpu_id], 1); }
static int xTaskResumeAll(void) {
  atomic_fetch_sub(&suspended[cpu_id], 1);
  return 0;
}
static void spin_lock(volatile spinlock_t* lock) {
  atomic_store(&waiting_local[cpu_id], 1);
  assert(pthread_mutex_lock((pthread_mutex_t*)&lock->native) == 0);
  atomic_store(&lock->owner, cpu_id);
  atomic_store(&waiting_local[cpu_id], 0);
}
static void spin_unlock(volatile spinlock_t* lock) {
  assert(atomic_load(&lock->owner) == cpu_id);
  atomic_store(&lock->owner, 3);
  assert(pthread_mutex_unlock((pthread_mutex_t*)&lock->native) == 0);
}
void bk_delay_us(uint32_t us) {
  (void)us;
  sched_yield();
}
bk_err_t bk_flash_shared_wait_ready(uint32_t timeout_ms);
void bk_flash_shared_dump(void);

/* SDK_SHARED_LOCK */

static pthread_mutex_t native_flash_mutex = PTHREAD_MUTEX_INITIALIZER;
static beken_mutex_t s_flash_mutex = &native_flash_mutex;
static uint32_t s_flash_lock_cpu_id = FLASH_SHARED_CPU_MAX;
static volatile uint8_t s_flash_irq_lock_depth[FLASH_SHARED_CPU_MAX];
static bk_err_t mb_flash_op_prepare(void) {
  if (overlap) {
    atomic_store(&writer_ready, 1);
    while (!atomic_load(&allow_writer)) sched_yield();
  }
  return BK_OK;
}
static bk_err_t mb_flash_op_finish(void) { return BK_OK; }

/* SDK_DRIVER_LOCK */

static void* writer(void* unused) {
  (void)unused;
  cpu_id = 1;
  flash_lock();
  uint32_t flags = bk_aspl_flash_enter_critical();
  bk_aspl_flash_exit_critical(flags);
  flash_unlock();
  atomic_store(&returned[1], 1);
  return NULL;
}
static void* reader(void* unused) {
  (void)unused;
  cpu_id = 2;
  uint32_t flags = bk_aspl_flash_enter_critical();
  bk_aspl_flash_exit_critical(flags);
  atomic_store(&returned[2], 1);
  return NULL;
}
static void* cp_worker(void* unused) {
  (void)unused;
  cpu_id = 0;
  uint32_t flags = rtos_disable_int();
  assert(bk_flash_shared_acquire(cpu_id) == BK_OK);
  bk_flash_shared_release(cpu_id);
  rtos_enable_int(flags);
  atomic_store(&returned[0], 1);
  return NULL;
}
static double now(void) {
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}
static void verify_released(void) {
  flash_shared_lock_t* shared = flash_shared_lock_get();
  assert(shared->owner_cpu == FLASH_SHARED_CPU_MAX);
  assert(atomic_load(&s_flash_aspl_spin_lock.owner) == FLASH_SHARED_CPU_MAX);
  for (unsigned i = 0; i < 3; ++i) {
    assert(atomic_load(&returned[i]) == 1);
    assert(shared->number[i] == 0 && shared->choosing[i] == 0);
    assert(s_flash_shared_rec_count[i] == 0);
    assert(atomic_load(&suspended[i]) == 0);
    assert(atomic_load(&irq_snapshot[i]) == 0);
  }
}

int main(int argc, char** argv) {
  setbuf(stdout, NULL);
  assert(bk_flash_shared_lock_init(true) == BK_OK);
  overlap = argc > 1 && strcmp(argv[1], "overlap") == 0;
  if (!overlap) {
    writer(NULL);
    reader(NULL);
    cp_worker(NULL);
    cpu_id = 1;
    uint32_t flags = rtos_disable_int();
    flash_lock();
    flash_lock();
    uint32_t nested = bk_aspl_flash_enter_critical();
    bk_aspl_flash_exit_critical(nested);
    flash_unlock();
    flash_unlock();
    rtos_enable_int(flags);
    assert(s_flash_irq_lock_depth[1] == 0);
    verify_released();
    puts("SEQUENTIAL_AND_IRQ_RECURSION_PASS");
    return 0;
  }
  pthread_t a, b, cp;
  double deadline = now() + 5;
  assert(pthread_create(&a, NULL, writer, NULL) == 0);
  while (!atomic_load(&writer_ready)) {
    assert(now() < deadline);
    sched_yield();
  }
  assert(pthread_create(&b, NULL, reader, NULL) == 0);
  flash_shared_lock_t* shared = flash_shared_lock_get();
  while (!shared->number[2]) {
    assert(now() < deadline);
    sched_yield();
  }
  assert(pthread_create(&cp, NULL, cp_worker, NULL) == 0);
  while (!shared->number[0]) {
    assert(now() < deadline);
    sched_yield();
  }
  assert(shared->owner_cpu == 1);
  atomic_store(&allow_writer, 1);
  double cycle_since = 0;
  while (!atomic_load(&returned[0]) || !atomic_load(&returned[1]) ||
         !atomic_load(&returned[2])) {
    if (shared->owner_cpu == 1 && atomic_load(&waiting_local[1]) &&
        atomic_load(&s_flash_aspl_spin_lock.owner) == 2 &&
        shared->number[2] != 0) {
      if (!cycle_since) cycle_since = now();
      if (now() - cycle_since >= 0.05) {
        fprintf(stderr, "LOCK_ORDER_CYCLE shared_owner=1 local_owner=2\n");
        /* Only this subprocess exits; no hardware is involved. */
        _Exit(86);
      }
    } else {
      cycle_since = 0;
    }
    assert(now() < deadline);
    sched_yield();
  }
  assert(pthread_join(a, NULL) == 0);
  assert(pthread_join(b, NULL) == 0);
  assert(pthread_join(cp, NULL) == 0);
  verify_released();
  puts("CONCURRENT_WRITER_READER_CP_PASS");
  return 0;
}
