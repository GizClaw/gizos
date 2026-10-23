#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct co_list_hdr {
  struct co_list_hdr* next;
};
struct co_list {
  struct co_list_hdr *first, *last;
};
typedef struct semaphore {
  unsigned count;
}* beken_semaphore_t;
typedef struct wdrv_cmd_cfm {
  struct co_list_hdr list;
  uint32_t waitcfm;
  uint16_t cfm_id, cfm_sn;
  uint8_t* cfm_buf;
  uint16_t cfm_len;
  beken_semaphore_t sema;
} wdrv_cmd_cfm;
typedef struct wdrv_cmd_hdr {
  uint32_t rsv0;
  uint16_t cmd_id, cmd_sn, rsv1, len;
} wdrv_cmd_hdr;
typedef struct wdrv_rx_msg {
  uint16_t id, cfm_sn, rsv, param_len;
  uint32_t pattern, param[1];
} wdrv_rx_msg;
static struct {
  struct co_list cfm_pending_list;
  pthread_mutex_t cfm_lock;
} wdrv_host_env = {.cfm_lock = PTHREAD_MUTEX_INITIALIZER};

enum {
  BK_OK = 0,
  WDRV_CMD_WAITCFM = 1,
  WDRV_CMD_NOWAITCFM = 0,
  WDRV_CMD_CFM_OFFSET = 0x8000,
  WDRV_CMDCFM_TIMEOUT = 2000,
  MAX_CMD_BUF_PAYLOAD = 800
};
static pthread_mutex_t monitor = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int worker_active, lock_attempted, transmitted, timeout_reply;
static unsigned live_semaphores, debug_calls;
static uint16_t last_id, last_sn;

#define BK_ASSERT(value) assert(value)
#define WDRV_LOGV(...) ((void)0)
#define WDRV_LOGD(...) ((void)0)
#define WDRV_LOGW(...) ((void)0)
#define WDRV_LOGE(...) ((void)0)
#define BK_LOGD(...) ((void)0)
#define WIFI_GLOBAL_INT_DECLARATION() ((void)0)
#define WIFI_GLOBAL_INT_DISABLE() ((void)0)
#define WIFI_GLOBAL_INT_RESTORE() ((void)0)
#define ASSERT_ERR(value) assert(value)
/* The SDK's TX and RX have different TU-local spinlocks. Local IRQ masking
 * cannot exclude the receiver on another core in this controlled schedule. */
#define WDRV_ENTER_TXMSG_CRITICAL(flags) ((flags) = 0)
#define WDRV_EXIT_TXMSG_CRITICAL(flags) ((void)(flags))

static int co_list_is_empty(const struct co_list* list) {
  return list->first == NULL;
}
static int rtos_lock_mutex(pthread_mutex_t* lock) {
  assert(pthread_mutex_lock(&monitor) == 0);
  if (worker_active && lock == &wdrv_host_env.cfm_lock) {
    lock_attempted = 1;
    assert(pthread_cond_broadcast(&changed) == 0);
  }
  assert(pthread_mutex_unlock(&monitor) == 0);
  return pthread_mutex_lock(lock);
}
static int rtos_unlock_mutex(pthread_mutex_t* lock) {
  return pthread_mutex_unlock(lock);
}
static int rtos_init_semaphore(beken_semaphore_t* out, unsigned maximum) {
  assert(maximum == 1);
  *out = calloc(1, sizeof(**out));
  assert(*out);
  ++live_semaphores;
  return BK_OK;
}
static int rtos_set_semaphore(beken_semaphore_t* semaphore) {
  ++(*semaphore)->count;
  return BK_OK;
}
static int rtos_get_semaphore(beken_semaphore_t* semaphore, unsigned timeout) {
  assert(timeout == WDRV_CMDCFM_TIMEOUT);
  if (!(*semaphore)->count) return -1;
  --(*semaphore)->count;
  return BK_OK;
}
static int rtos_deinit_semaphore(beken_semaphore_t* semaphore) {
  assert(live_semaphores);
  --live_semaphores;
  free(*semaphore);
  *semaphore = NULL;
  return BK_OK;
}
static void wdrv_print_debug_info(void) { ++debug_calls; }
static void wdrv_cntrl_get_cif_stats(void) { ++debug_calls; }
void wdrv_rx_confirm_tx_msg(wdrv_rx_msg* message);
static int wdrv_tx_msg_send(uint8_t* message, uint16_t length, uint8_t wait) {
  assert(length == sizeof(wdrv_cmd_hdr) && wait == WDRV_CMD_WAITCFM);
  const wdrv_cmd_hdr* header = (const wdrv_cmd_hdr*)message;
  last_id = header->cmd_id + WDRV_CMD_CFM_OFFSET;
  last_sn = header->cmd_sn;
  assert(pthread_mutex_lock(&monitor) == 0);
  transmitted = 1;
  assert(pthread_cond_broadcast(&changed) == 0);
  assert(pthread_mutex_unlock(&monitor) == 0);
  if (!timeout_reply) {
    wdrv_rx_msg reply = {.id = last_id,
                         .cfm_sn = last_sn,
                         .param_len = sizeof(uint32_t),
                         .param = {42}};
    /* Immediate RX also proves enqueue released cfm_lock before send/wait. */
    wdrv_rx_confirm_tx_msg(&reply);
  }
  return BK_OK;
}

/* SDK_FUNCTIONS */

static uint32_t response;
static int send_result;
static void* sender(void* unused) {
  (void)unused;
  wdrv_cmd_hdr request = {.cmd_id = 0x123};
  wdrv_cmd_cfm confirm = {.waitcfm = WDRV_CMD_WAITCFM};
  send_result = wdrv_tx_msg((uint8_t*)&request, sizeof(request), &confirm,
                            (uint8_t*)&response);
  return NULL;
}
int main(void) {
  wdrv_cmd_cfm older = {.cfm_id = 0xffff, .cfm_sn = 0xffff};
  wdrv_host_env.cfm_pending_list.first = &older.list;
  wdrv_host_env.cfm_pending_list.last = &older.list;
  /* RX owns its real queue mutex while a different core starts a command. */
  assert(pthread_mutex_lock(&wdrv_host_env.cfm_lock) == 0);
  worker_active = 1;
  pthread_t worker;
  assert(pthread_create(&worker, NULL, sender, NULL) == 0);
  assert(pthread_mutex_lock(&monitor) == 0);
  while (!lock_attempted && !transmitted)
    assert(pthread_cond_wait(&changed, &monitor) == 0);
  if (transmitted) {
    fputs("command transmitted while receiver owns queue\n", stderr);
    return 1;
  }
  assert(pthread_mutex_unlock(&monitor) == 0);
  assert(pthread_mutex_unlock(&wdrv_host_env.cfm_lock) == 0);
  assert(pthread_join(worker, NULL) == 0);
  worker_active = 0;
  assert(send_result == (int)sizeof(uint32_t) && response == 42);
  assert(wdrv_host_env.cfm_pending_list.first == &older.list);
  assert(wdrv_host_env.cfm_pending_list.last == &older.list);
  assert(older.list.next == NULL && live_semaphores == 0);

  /* A missing reply removes only its own waiter and frees its semaphore. */
  timeout_reply = 1;
  response = 0;
  sender(NULL);
  assert(send_result == -3 && response == 0 && debug_calls == 2);
  assert(wdrv_host_env.cfm_pending_list.first == &older.list);
  assert(wdrv_host_env.cfm_pending_list.last == &older.list);
  assert(older.list.next == NULL && live_semaphores == 0);
  wdrv_rx_msg late = {.id = last_id,
                      .cfm_sn = last_sn,
                      .param_len = sizeof(uint32_t),
                      .param = {99}};
  wdrv_rx_confirm_tx_msg(&late);
  assert(response == 0 && older.list.next == NULL);
  puts(
      "WDRV confirmation exclusion, immediate reply, timeout and late reply "
      "PASS");
  return 0;
}
