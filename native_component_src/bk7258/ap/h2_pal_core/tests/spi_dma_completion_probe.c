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
typedef struct {
  unsigned count;
} semaphore_t;
typedef struct {
  int hal;
  bool is_tx_blocked;
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

static spi_state_t s_spi[2];
static callback_t s_spi_tx_finish_isr[2], s_spi_rx_finish_isr[2];
static volatile spi_id_t s_current_spi_dma_wr_id, s_current_spi_dma_rd_id;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned started, signals, timeouts, callbacks, expected_callback;
static unsigned first_port, next_port;
static bool transmitting;
static int worker_result;

#define BK_OK 0
#define BEKEN_NEVER_TIMEOUT UINT32_MAX
#define SPI_MAX_LENGTH 65535
#define SPI_LOGV(...) ((void)0)
#define SPI_LOGE(...) ((void)0)
#define SPI_RETURN_ON_INVALID_ID(id) assert((id) < 2u)
#define SPI_RETURN_ON_ID_NOT_INIT(id) ((void)(id))
#define BK_RETURN_ON_NULL(data) assert((data) != NULL)

static uint32_t spi_enter_critical(void) { return 0u; }
static void spi_exit_critical(uint32_t level) { (void)level; }
static void bk_delay_us(unsigned delay) { (void)delay; }
static void bk_dma_stop(dma_id_t id) { (void)id; }
static void transfer_started(dma_id_t id, void* data, uint32_t size) {
  (void)id;
  (void)data;
  assert(size == 4u);
  assert(pthread_mutex_lock(&lock) == 0);
  ++started;
  assert(pthread_cond_broadcast(&changed) == 0);
  assert(pthread_mutex_unlock(&lock) == 0);
}
static void bk_dma_write(dma_id_t id, void* data, uint32_t size) {
  transfer_started(id, data, size);
}
static void bk_dma_read(dma_id_t id, void* data, uint32_t size) {
  transfer_started(id, data, size);
}

static int rtos_set_semaphore(semaphore_t* semaphore) {
  assert(pthread_mutex_lock(&lock) == 0);
  ++semaphore->count;
  ++signals;
  assert(pthread_cond_broadcast(&changed) == 0);
  /* CPU1 can consume completion and enter its next real SDK transfer while
   * CPU0 is still in this ISR. Hold the interrupt tail at exactly that cut. */
  while (signals == 1u && started < 2u)
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
HAL_NOOP(spi_hal_clear_tx_fifo)
HAL_NOOP(spi_hal_enable_tx)
HAL_NOOP(spi_hal_disable_tx)
HAL_NOOP(spi_hal_enable_tx_fifo_int)
HAL_NOOP(spi_hal_disable_tx_fifo_int)
HAL_NOOP(spi_hal_clear_tx_fifo_int_status)
HAL_NOOP(spi_hal_clear_rx_fifo)
HAL_NOOP(spi_hal_enable_rx)
HAL_NOOP(spi_hal_disable_rx)
HAL_NOOP(spi_hal_disable_rx_fifo_int)
HAL_NOOP(spi_hal_disable_rx_overflow_int)
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
  assert(port == expected_callback);
  assert((uintptr_t)user == port);
  ++callbacks;
}

static void* worker(void* user) {
  (void)user;
  uint8_t bytes[4] = {0};
  worker_result = transmitting
                      ? bk_spi_dma_write_bytes(first_port, bytes, sizeof(bytes))
                      : bk_spi_dma_read_bytes(first_port, bytes, sizeof(bytes));
  if (worker_result == BK_OK)
    worker_result =
        transmitting ? bk_spi_dma_write_bytes(next_port, bytes, sizeof(bytes))
                     : bk_spi_dma_read_bytes(next_port, bytes, sizeof(bytes));
  return NULL;
}

static void run_pair(unsigned first, unsigned next) {
  memset(s_spi, 0, sizeof(s_spi));
  started = signals = timeouts = callbacks = 0u;
  first_port = first;
  next_port = next;
  for (unsigned port = 0u; port < 2u; ++port) {
    s_spi[port].spi_tx_dma_chan = 2u * port;
    s_spi[port].spi_rx_dma_chan = 2u * port + 1u;
    s_spi_tx_finish_isr[port] = s_spi_rx_finish_isr[port] =
        (callback_t){on_complete, (void*)(uintptr_t)port};
  }
  pthread_t task;
  assert(pthread_create(&task, NULL, worker, NULL) == 0);
  assert(pthread_mutex_lock(&lock) == 0);
  while (started < 1u) assert(pthread_cond_wait(&changed, &lock) == 0);
  assert(pthread_mutex_unlock(&lock) == 0);
  expected_callback = first;
  if (transmitting)
    spi_dma_tx_finish_handler(s_spi[first].spi_tx_dma_chan);
  else
    spi_dma_rx_finish_handler(s_spi[first].spi_rx_dma_chan);
  expected_callback = next;
  if (transmitting)
    spi_dma_tx_finish_handler(s_spi[next].spi_tx_dma_chan);
  else
    spi_dma_rx_finish_handler(s_spi[next].spi_rx_dma_chan);
  assert(pthread_join(task, NULL) == 0);
  if (timeouts != 0u || signals != 2u || callbacks != 2u ||
      worker_result != BK_OK) {
    fprintf(stderr,
            "second DMA completion lost: %s %u->%u signals=%u timeouts=%u\n",
            transmitting ? "TX" : "RX", first, next, signals, timeouts);
    exit(1);
  }
  printf("PASS %s %u->%u: both SDK transfers completed\n",
         transmitting ? "TX" : "RX", first, next);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  assert(strcmp(argv[1], "tx") == 0 || strcmp(argv[1], "rx") == 0);
  transmitting = strcmp(argv[1], "tx") == 0;
  for (unsigned first = 0u; first < 2u; ++first)
    for (unsigned next = 0u; next < 2u; ++next) run_pair(first, next);
  return 0;
}
