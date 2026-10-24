#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef unsigned spi_id_t;
typedef unsigned dma_id_t;
typedef int bk_err_t;
typedef int spi_hal_t;
typedef struct {
  unsigned count;
} semaphore_t;
typedef struct {
  int hal;
  uint8_t* tx_buf;
  uint32_t tx_size;
  uint8_t* rx_buf;
  uint32_t rx_size;
  uint32_t rx_offset;
  bool is_sw_tx_finished;
  bool is_tx_blocked;
  bool h2_tx_dma_active;
  bool h2_rx_dma_active;
  bool is_rx_blocked;
  dma_id_t spi_tx_dma_chan;
  dma_id_t spi_rx_dma_chan;
  semaphore_t tx_sema;
  semaphore_t rx_sema;
} spi_state_t;
typedef struct {
  void (*callback)(spi_id_t, void*);
  void* param;
} callback_t;
typedef callback_t spi_callback_t;

static spi_state_t s_spi[2];
static callback_t s_spi_tx_finish_isr[2], s_spi_rx_finish_isr[2];
static callback_t s_spi_rx_isr[2];
static volatile spi_id_t s_current_spi_dma_wr_id, s_current_spi_dma_rd_id;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned started, signals, timeouts, callbacks, expected_callback;
static unsigned first_port, next_port;
static bool transmitting;
static bool first_dma, next_dma, slave;
static bool inject_ordinary_irq;
static bool handoff_cut = true;
static bool duplex_retry;
static pthread_mutex_t spi_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned spi_lock_depth;
static bool is_xfer_overflow;
static unsigned interrupt_status, late_teardowns, generation[2];
static _Thread_local bool dma_operation;
static _Thread_local unsigned interrupt_generation;
static int worker_result;

#define BK_OK 0
#define BEKEN_NEVER_TIMEOUT UINT32_MAX
#define SPI_MAX_LENGTH 65535
#define SPI_LOGV(...) ((void)0)
#define SPI_LOGE(...) ((void)0)
#define SPI_LOGW(...) ((void)0)
#define SPI_LOGI(...) ((void)0)
#define BK_ERR_SPI_DUPLEX_SIZE_NOT_EQUAL (-2)
#define BK_ERR_SPI_RX_TIMEOUT (-3)
#define SPI_STATIS_DEC() ((void)0)
#define SPI_STATIS_GET(...) ((void)0)
#define SPI_STATIS_INC(...) ((void)0)
#define SPI_RETURN_ON_NOT_INIT() ((void)0)
#define SPI_PM_CHECK_RESTORE(id) ((void)(id))
#define CONFIG_SPI_SUPPORT_TX_FIFO_WR_READY 1
#define SPI_RETURN_ON_INVALID_ID(id) assert((id) < 2u)
#define SPI_RETURN_ON_ID_NOT_INIT(id) ((void)(id))
#define BK_RETURN_ON_NULL(data) assert((data) != NULL)

static uint32_t spi_enter_critical(void) {
  assert(pthread_mutex_lock(&spi_lock) == 0);
  assert(spi_lock_depth++ == 0u);
  return 0u;
}
static void spi_exit_critical(uint32_t level) {
  (void)level;
  assert(spi_lock_depth-- == 1u);
  assert(pthread_mutex_unlock(&spi_lock) == 0);
}
static void bk_delay_us(unsigned delay) { (void)delay; }
static void bk_dma_stop(dma_id_t id) { (void)id; }
static void transfer_started(dma_id_t id, void* data, uint32_t size) {
  (void)data;
  assert(size == 4u);
  assert(pthread_mutex_lock(&lock) == 0);
  ++started;
  assert(id / 2u < 2u);
  generation[id / 2u] = started;
  assert(pthread_cond_broadcast(&changed) == 0);
  assert(pthread_mutex_unlock(&lock) == 0);
}
static void bk_dma_write(dma_id_t id, void* data, uint32_t size) {
  transfer_started(id, data, size);
}
static void bk_dma_read(dma_id_t id, void* data, uint32_t size) {
  transfer_started(id, data, size);
}

static void bk_dma_start(dma_id_t id) { transfer_started(id, NULL, 4u); }
static void bk_dma_set_dest_start_addr(dma_id_t id, uint32_t address) {
  (void)id;
  (void)address;
}
static void bk_dma_set_src_start_addr(dma_id_t id, uint32_t address) {
  (void)id;
  (void)address;
}
static void bk_dma_set_transfer_len(dma_id_t id, uint32_t size) {
  (void)id;
  assert(size == 4u);
}
static uint32_t dma_wait_to_idle(dma_id_t id) {
  (void)id;
  return 0u;
}

static int rtos_set_semaphore(semaphore_t* semaphore) {
  assert(spi_lock_depth == 0u);
  assert(pthread_mutex_lock(&lock) == 0);
  ++semaphore->count;
  ++signals;
  assert(pthread_cond_broadcast(&changed) == 0);
  /* CPU1 can consume completion and enter its next real SDK transfer while
   * CPU0 is still in this ISR. Hold the interrupt tail at exactly that cut. */
  while (handoff_cut && signals == 1u && started < 2u)
    assert(pthread_cond_wait(&changed, &lock) == 0);
  assert(pthread_mutex_unlock(&lock) == 0);
  return BK_OK;
}

static int rtos_get_semaphore(semaphore_t* semaphore, uint32_t timeout) {
  (void)timeout;
  struct timespec deadline;
  assert(timespec_get(&deadline, TIME_UTC) == TIME_UTC);
  ++deadline.tv_sec;
  assert(pthread_mutex_lock(&lock) == 0);
  while (semaphore->count == 0u) {
    int rc = pthread_cond_timedwait(&changed, &lock, &deadline);
    if (rc == ETIMEDOUT) {
      ++timeouts;
      assert(pthread_mutex_unlock(&lock) == 0);
      return -1;
    }
    assert(rc == 0);
  }
  --semaphore->count;
  assert(pthread_mutex_unlock(&lock) == 0);
  return BK_OK;
}

#define HAL_NOOP(name) \
  static void name(int* hal) { (void)hal; }
HAL_NOOP(spi_hal_enable_tx_rx)
HAL_NOOP(spi_hal_clear_tx_fifo)
HAL_NOOP(spi_hal_enable_tx)
HAL_NOOP(spi_hal_enable_tx_fifo_int)
HAL_NOOP(spi_hal_disable_tx_fifo_int)
HAL_NOOP(spi_hal_clear_tx_fifo_int_status)
HAL_NOOP(spi_hal_clear_rx_fifo)
HAL_NOOP(spi_hal_disable_rx)
HAL_NOOP(spi_hal_disable_rx_fifo_int)
HAL_NOOP(spi_hal_enable_rx_fifo_int)
HAL_NOOP(spi_hal_disable_rx_overflow_int)
HAL_NOOP(spi_hal_enable_rx_finish_int)
static void spi_hal_disable_tx(int* hal) {
  assert(pthread_mutex_lock(&lock) == 0);
  if (interrupt_generation && generation[*hal] > interrupt_generation)
    ++late_teardowns;
  assert(pthread_mutex_unlock(&lock) == 0);
}
static void spi_hal_enable_rx(int* hal) {
  if (!dma_operation)
    transfer_started(2u * (unsigned)*hal + 1u, s_spi[*hal].rx_buf, 4u);
}
static void spi_id_write_bytes_common(spi_id_t id) {
  transfer_started(2u * id, s_spi[id].tx_buf, s_spi[id].tx_size);
}
static void spi_id_read_bytes_common(spi_id_t id) { (void)id; }
static int spi_hal_read_byte(int* hal, uint8_t* byte) {
  (void)hal;
  (void)byte;
  return -1;
}
static bool spi_hal_is_master(int* hal) {
  (void)hal;
  return !slave;
}
static bool spi_hal_is_slave(int* hal) {
  (void)hal;
  return slave;
}
static uint32_t spi_hal_get_interrupt_status(int* hal) {
  (void)hal;
  return interrupt_status;
}
static void spi_hal_clear_interrupt_status(int* hal, uint32_t status) {
  (void)hal;
  (void)status;
}
#define IRQ_QUERY(name, mask)                   \
  static bool name(int* hal, uint32_t status) { \
    (void)hal;                                  \
    return (status & (mask)) != 0u;             \
  }
IRQ_QUERY(spi_hal_is_rx_fifo_int_triggered_with_status, 8u)
IRQ_QUERY(spi_hal_is_rx_finish_int_triggered, 1u)
IRQ_QUERY(spi_hal_is_rx_overflow_int_triggered, 0u)
IRQ_QUERY(spi_hal_is_tx_fifo_int_triggered_with_status, 16u)
IRQ_QUERY(spi_hal_is_tx_finish_int_triggered, 2u)
IRQ_QUERY(spi_hal_is_tx_underflow_int_triggered, 0u)
IRQ_QUERY(spi_hal_is_slave_release_int_triggered, 4u)
static void spi_hal_set_tx_trans_len(int* hal, uint32_t size) {
  (void)hal;
  (void)size;
}
static void spi_hal_set_rx_trans_len(int* hal, uint32_t size) {
  (void)hal;
  (void)size;
}
static bool spi_hal_is_tx_fifo_int_triggered(int* hal) {
  (void)hal;
  return true;
}

/* SDK_FUNCTIONS */

static void on_complete(spi_id_t port, void* user) {
  assert(spi_lock_depth == 0u);
  assert(port == expected_callback);
  assert((uintptr_t)user == port);
  ++callbacks;
}

static void* worker(void* user) {
  (void)user;
  uint8_t bytes[4] = {0};
  for (unsigned index = 0u; index < 2u; ++index) {
    unsigned port = index ? next_port : first_port;
    dma_operation = index ? next_dma : first_dma;
    if (dma_operation)
      worker_result = transmitting
                          ? bk_spi_dma_write_bytes(port, bytes, sizeof(bytes))
                          : bk_spi_dma_read_bytes(port, bytes, sizeof(bytes));
    else
      worker_result = transmitting
                          ? bk_spi_write_bytes(port, bytes, sizeof(bytes))
                          : bk_spi_read_bytes(port, bytes, sizeof(bytes));
    if (worker_result != BK_OK) break;
  }
  return NULL;
}

static void complete(bool dma, unsigned port, unsigned number) {
  interrupt_generation = number;
  expected_callback = port;
  if (!dma) {
    interrupt_status = slave ? 4u : transmitting ? 2u : 1u;
    spi_isr_common(port);
  } else if (transmitting) {
    spi_dma_tx_finish_handler(s_spi[port].spi_tx_dma_chan);
  } else {
    spi_dma_rx_finish_handler(s_spi[port].spi_rx_dma_chan);
  }
  interrupt_generation = 0u;
}

static void run_pair(unsigned first, unsigned next) {
  memset(s_spi, 0, sizeof(s_spi));
  memset(generation, 0, sizeof(generation));
  started = signals = timeouts = callbacks = 0u;
  late_teardowns = 0u;
  first_port = first;
  next_port = next;
  for (unsigned port = 0u; port < 2u; ++port) {
    s_spi[port].hal = (int)port;
    s_spi[port].spi_tx_dma_chan = 2u * port;
    s_spi[port].spi_rx_dma_chan = 2u * port + 1u;
    const callback_t callback = {on_complete, (void*)(uintptr_t)port};
    s_spi_tx_finish_isr[port] = transmitting ? callback : (callback_t){0};
    s_spi_rx_finish_isr[port] = transmitting ? (callback_t){0} : callback;
  }
  pthread_t task;
  assert(pthread_create(&task, NULL, worker, NULL) == 0);
  assert(pthread_mutex_lock(&lock) == 0);
  while (started < 1u) assert(pthread_cond_wait(&changed, &lock) == 0);
  assert(pthread_mutex_unlock(&lock) == 0);
  complete(first_dma, first, 1u);
  if (inject_ordinary_irq && next_dma) {
    const unsigned before_signals = signals, before_callbacks = callbacks;
    interrupt_generation = 2u;
    interrupt_status = (transmitting ? 2u | 16u : 1u | 8u) | (slave ? 4u : 0u);
    spi_isr_common(next);
    if (signals != before_signals || callbacks != before_callbacks) {
      fprintf(
          stderr,
          "ordinary SPI IRQ consumed DMA completion: signals=%u callbacks=%u\n",
          signals, callbacks);
      exit(2);
    }
  }
  complete(next_dma, next, 2u);
  assert(pthread_join(task, NULL) == 0);
  /* Slave RX CS-release wakes the reader without an RX_FINISH callback.
   * Preserve callback delivery as well as the two blocking completions. */
  const unsigned expected_callbacks =
      (first_dma || !slave || transmitting ? 1u : 0u) +
      (next_dma || !slave || transmitting ? 1u : 0u);
  if (timeouts != 0u || signals != 2u || callbacks != expected_callbacks ||
      late_teardowns != 0u || worker_result != BK_OK) {
    fprintf(
        stderr,
        "second SPI completion lost: %s %u->%u signals=%u timeouts=%u teardown=%u\n",
        transmitting ? "TX" : "RX", first, next, signals, timeouts,
        late_teardowns);
    exit(1);
  }
  printf("PASS %s %s %s->%s %u->%u: both SDK transfers completed\n",
         transmitting ? "TX" : "RX", slave ? "slave" : "master",
         first_dma ? "DMA" : "CPU", next_dma ? "DMA" : "CPU", first, next);
}

static void wait_started(unsigned count) {
  assert(pthread_mutex_lock(&lock) == 0);
  while (started < count) assert(pthread_cond_wait(&changed, &lock) == 0);
  assert(pthread_mutex_unlock(&lock) == 0);
}

static void* duplex_worker(void* user) {
  uint8_t tx[4] = {0}, rx[4] = {0};
  dma_operation = true;
  worker_result = bk_spi_dma_duplex_xfer((spi_id_t)(uintptr_t)user, tx,
                                         sizeof(tx), rx, sizeof(rx));
  return NULL;
}

static void run_duplex(unsigned port) {
  memset(s_spi, 0, sizeof(s_spi));
  memset(generation, 0, sizeof(generation));
  started = signals = timeouts = callbacks = late_teardowns = 0u;
  is_xfer_overflow = false;
  handoff_cut = false;
  expected_callback = port;
  for (unsigned i = 0u; i < 2u; ++i) {
    s_spi[i].hal = (int)i;
    s_spi[i].spi_tx_dma_chan = 2u * i;
    s_spi[i].spi_rx_dma_chan = 2u * i + 1u;
    s_spi[i].is_sw_tx_finished = true;
    s_spi_tx_finish_isr[i] = (callback_t){on_complete, (void*)(uintptr_t)i};
    s_spi_rx_finish_isr[i] = (callback_t){on_complete, (void*)(uintptr_t)i};
  }
  pthread_t task;
  assert(pthread_create(&task, NULL, duplex_worker, (void*)(uintptr_t)port) ==
         0);
  const unsigned attempts = duplex_retry ? 2u : 1u;
  for (unsigned attempt = 0u; attempt < attempts; ++attempt) {
    wait_started(2u * (attempt + 1u));
    if (duplex_retry && attempt == 0u) {
      const uint32_t flags = spi_enter_critical();
      is_xfer_overflow = true;
      spi_exit_critical(flags);
    }
    interrupt_status = 1u | 2u | 8u | 16u | (slave ? 4u : 0u);
    spi_isr_common(port);
    if (signals != 2u * attempt || callbacks != 2u * attempt) {
      fprintf(stderr, "ordinary SPI IRQ consumed duplex DMA completion\n");
      exit(2);
    }
    transmitting = true;
    complete(true, port, 1u);
    transmitting = false;
    complete(true, port, 2u);
  }
  assert(pthread_join(task, NULL) == 0);
  assert(worker_result == BK_OK && timeouts == 0u);
  assert(signals == 2u * attempts && callbacks == 2u * attempts);
  assert(!s_spi[port].h2_tx_dma_active && !s_spi[port].h2_rx_dma_active);
  printf("PASS duplex %s port=%u retry=%u: DMA owns both completions\n",
         slave ? "slave" : "master", port, duplex_retry);
}

int main(int argc, char** argv) {
  if (argc == 4 && strcmp(argv[1], "duplex") == 0) {
    slave = strcmp(argv[2], "slave") == 0;
    duplex_retry = strcmp(argv[3], "retry") == 0;
    for (unsigned port = 0u; port < 2u; ++port) run_duplex(port);
    return 0;
  }
  assert(argc == 5 || argc == 6);
  assert(strcmp(argv[1], "tx") == 0 || strcmp(argv[1], "rx") == 0);
  transmitting = strcmp(argv[1], "tx") == 0;
  first_dma = strcmp(argv[2], "dma") == 0;
  next_dma = strcmp(argv[3], "dma") == 0;
  slave = strcmp(argv[4], "slave") == 0;
  inject_ordinary_irq = argc > 5 && strcmp(argv[5], "ordinary") == 0;
  for (unsigned first = 0u; first < 2u; ++first)
    for (unsigned next = 0u; next < 2u; ++next) run_pair(first, next);
  return 0;
}
