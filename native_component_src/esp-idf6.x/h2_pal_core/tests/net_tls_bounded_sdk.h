#ifndef H2_ESP_NET_TLS_BOUNDED_TEST_SDK_H
#define H2_ESP_NET_TLS_BOUNDED_TEST_SDK_H

#include "net/h2_pal_net.h"
#include "net/h2_pal_netif.h"
#include "h2_esp_platform_net_socket.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* SDK effects only. The harness compiles the production TLS helpers, legacy
 * wrapper, bounded wrapper and close path; no state machine is copied here. */
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef struct test_semaphore {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  int available, waiters, is_static;
}* SemaphoreHandle_t;
typedef struct test_semaphore StaticSemaphore_t;
typedef uint8_t StackType_t;
typedef void (*TaskFunction_t)(void*);
typedef struct {
  pthread_t thread;
  TaskFunction_t entry;
  void* user;
  int role;
} StaticTask_t;
#define portBYTE_ALIGNMENT 16u
#define tskNO_AFFINITY (-1)
typedef pthread_t* TaskHandle_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) assert(pthread_mutex_lock(lock) == 0)
#define portEXIT_CRITICAL(lock) assert(pthread_mutex_unlock(lock) == 0)
#define CONFIG_LWIP_MAX_SOCKETS 32u
#define LWIP_IPV6 1
#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 0x7fff
#endif
#define configTICK_RATE_HZ 1000u
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdPASS 1
#define tskIDLE_PRIORITY 0u
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define MBEDTLS_PRIVATE(field) field
#ifndef MBEDTLS_VERSION_MAJOR
#define MBEDTLS_VERSION_MAJOR 4
#endif
#define MBEDTLS_SSL_IS_CLIENT 0
#define MBEDTLS_SSL_TRANSPORT_STREAM 0
#define MBEDTLS_SSL_PRESET_DEFAULT 0
#define MBEDTLS_SSL_VERIFY_NONE 0
#define MBEDTLS_SSL_VERIFY_REQUIRED 1
#define MBEDTLS_ERR_SSL_WANT_READ (-100)
#define MBEDTLS_ERR_SSL_WANT_WRITE (-101)
#define MBEDTLS_ERR_NET_SEND_FAILED (-102)
#define MBEDTLS_ERR_NET_RECV_FAILED (-103)
#define MBEDTLS_ERR_X509_CERT_VERIFY_FAILED (-104)
#define MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY (-105)
#define MBEDTLS_X509_BADCERT_OTHER 1u
#define MBEDTLS_X509_BADCERT_FUTURE 2u
#define MBEDTLS_X509_BADCERT_EXPIRED 4u
#define PSA_SUCCESS 0

typedef struct mbedtls_x509_time {
  int year, mon, day, hour, min, sec;
} mbedtls_x509_time;
typedef struct mbedtls_x509_crt {
  struct mbedtls_x509_crt* self;
  mbedtls_x509_time valid_from, valid_to;
} mbedtls_x509_crt;
typedef int (*test_verify_fn)(void*, mbedtls_x509_crt*, int, uint32_t*);
typedef struct mbedtls_ssl_config {
  struct mbedtls_ssl_config* self;
  test_verify_fn f_vrfy;
  void* p_vrfy;
  const char** alpn;
  mbedtls_x509_crt* ca;
} mbedtls_ssl_config;
typedef struct mbedtls_ssl_context {
  struct mbedtls_ssl_context* self;
  mbedtls_ssl_config* config;
  void* bio;
  int (*send_fn)(void*, const unsigned char*, size_t);
  int (*recv_fn)(void*, unsigned char*, size_t);
  int phase;
} mbedtls_ssl_context;
typedef struct {
  test_verify_fn delegate;
  void* delegate_user;
  int certificate_bundle;
} h2_esp_net_tls_verify_t;

static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_changed = PTHREAD_COND_INITIALIZER;
static _Thread_local int test_is_worker;
static int test_gate_stage, test_gate_entered, test_gate_release,
    test_gate_primary_only;
static int test_fail_task, test_task_count, test_allocations, test_alloc_fail;
static int test_dynamic_task_calls, test_static_task_attempts,
    test_alloc_persistent;
static unsigned test_static_delay_ms;
static _Atomic int64_t test_clock_fixed_us, test_clock_resume_us;
static _Atomic int test_early_delay_return, test_early_delay_observed;
static int test_emulate_dontwait_fd = -1, test_plain_send_entered;
static int test_hold_recv_fd = -1, test_recv_held, test_recv_release;
static int test_write_wait_fd = -1, test_write_wait_entered;
static int test_send_prefix_fd = -1;
static size_t test_send_prefix_length;
static int test_hold_cancel_claim, test_cancel_claim_entered,
    test_cancel_claim_release;
static const unsigned char* test_ssl_write_pointer;
static size_t test_ssl_write_length;
static int test_ssl_write_pending, test_ssl_write_retries;
static int test_parse_fail, test_ca_checks, test_sni_checks, test_alpn_checks;
static int test_contexts, test_io_calls, test_closed;
static int test_probe_send_fd = -1, test_probe_send_flags,
           test_probe_send_result;
static int test_probe_recv_fd = -1, test_probe_recv_flags,
           test_probe_recv_result;
static int64_t test_probe_send_start, test_probe_send_end,
    test_probe_recv_start, test_probe_recv_end;
static int test_native_connect_calls, test_native_accept_calls;
static int test_caller_allocations, test_worker_allocations, test_sem_creates,
    test_sem_deletes;
static size_t test_expected_ca_length = 4u, test_max_allocation;
static int test_copy_gate, test_copy_entered, test_copy_release;
static int test_publish_block, test_publish_held, test_publish_release;
static int test_listener_shutdown_noop;
static int test_simulate_fin_wait, test_expect_normal_fd = -1;
static int test_fin_wait_fd = -1;
static int test_fail_normal_linger, test_normal_linger_attempts;
static int test_fail_abortive_linger, test_hold_accept, test_accept_entered,
    test_accept_release;
static int test_last_accepted_fd = -1;
static int test_accept_would_block, test_fail_accepted_fcntl_command;
static uint64_t test_linger_first_failure_ms, test_linger_last_failure_ms;
static int test_hold_cleanup_claim, test_cleanup_claim_entered,
    test_cleanup_claim_release;
static SemaphoreHandle_t test_registry;
static pthread_t test_publish_thread;
static void test_sdk_gate(int stage);

static int64_t esp_timer_get_time(void) {
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  int64_t value = (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
  return value < atomic_load(&test_clock_resume_us)
             ? atomic_load(&test_clock_fixed_us)
             : value;
}

static struct timespec test_after_ms(uint32_t ms) {
  struct timespec time;
  assert(clock_gettime(CLOCK_REALTIME, &time) == 0);
  time.tv_sec += ms / 1000u;
  time.tv_nsec += (long)(ms % 1000u) * 1000000;
  if (time.tv_nsec >= 1000000000) {
    ++time.tv_sec;
    time.tv_nsec -= 1000000000;
  }
  return time;
}

static SemaphoreHandle_t test_semaphore_create(int available) {
  test_sdk_gate(9);
  ++test_sem_creates;
  SemaphoreHandle_t sem = calloc(1, sizeof(*sem));
  assert(sem != NULL);
  assert(pthread_mutex_init(&sem->mutex, NULL) == 0);
  assert(pthread_cond_init(&sem->changed, NULL) == 0);
  sem->available = available;
  return sem;
}
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(
    StaticSemaphore_t* storage) {
  memset(storage, 0, sizeof(*storage));
  assert(pthread_mutex_init(&storage->mutex, NULL) == 0);
  assert(pthread_cond_init(&storage->changed, NULL) == 0);
  storage->available = 1;
  storage->is_static = 1;
  ++test_sem_creates;
  test_registry = storage;
  return storage;
}
static SemaphoreHandle_t xSemaphoreCreateMutex(void) {
  return test_semaphore_create(1);
}
static SemaphoreHandle_t xSemaphoreCreateBinary(void) {
  return test_semaphore_create(0);
}
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(
    StaticSemaphore_t* storage) {
  memset(storage, 0, sizeof(*storage));
  assert(pthread_mutex_init(&storage->mutex, NULL) == 0);
  assert(pthread_cond_init(&storage->changed, NULL) == 0);
  storage->is_static = 1;
  ++test_sem_creates;
  return storage;
}
static int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
  assert(sem != NULL);
  assert(pthread_mutex_lock(&sem->mutex) == 0);
  struct timespec until =
      test_after_ms(ticks == portMAX_DELAY ? 10000u : ticks);
  while (!sem->available) {
    if (ticks == 0u) break;
    ++sem->waiters;
    int rc = pthread_cond_timedwait(&sem->changed, &sem->mutex, &until);
    --sem->waiters;
    assert(rc == 0 || rc == ETIMEDOUT);
    if (rc == ETIMEDOUT) {
      assert(ticks != portMAX_DELAY);
      break;
    }
  }
  int acquired = sem->available;
  if (acquired) sem->available = 0;
  assert(pthread_mutex_unlock(&sem->mutex) == 0);
  if (acquired && test_is_worker == 2 && ticks == portMAX_DELAY) {
    test_sdk_gate(14);
    assert(pthread_mutex_lock(&test_lock) == 0);
    if (test_hold_cancel_claim) {
      test_cancel_claim_entered = 1;
      assert(pthread_cond_broadcast(&test_changed) == 0);
      while (!test_cancel_claim_release)
        assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
    }
    assert(pthread_mutex_unlock(&test_lock) == 0);
  }
  if (acquired && test_is_worker == 3 && ticks == portMAX_DELAY) {
    assert(pthread_mutex_lock(&test_lock) == 0);
    if (test_hold_cleanup_claim) {
      test_cleanup_claim_entered = 1;
      assert(pthread_cond_broadcast(&test_changed) == 0);
      while (!test_cleanup_claim_release)
        assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
    }
    assert(pthread_mutex_unlock(&test_lock) == 0);
  }
  return acquired;
}
static void vTaskDelay(TickType_t ticks) {
  if (ticks == 1u && atomic_exchange(&test_early_delay_return, 0)) {
    atomic_store(&test_early_delay_observed, 1);
    return;
  }
  struct timespec pause = {ticks / 1000u, (long)(ticks % 1000u) * 1000000};
  assert(nanosleep(&pause, NULL) == 0);
}
static int xSemaphoreGive(SemaphoreHandle_t sem) {
  assert(pthread_mutex_lock(&sem->mutex) == 0);
  sem->available = 1;
  assert(pthread_cond_broadcast(&sem->changed) == 0);
  assert(pthread_mutex_unlock(&sem->mutex) == 0);
  return pdTRUE;
}
static void vSemaphoreDelete(SemaphoreHandle_t sem) {
  ++test_sem_deletes;
  assert(pthread_cond_destroy(&sem->changed) == 0);
  assert(pthread_mutex_destroy(&sem->mutex) == 0);
  if (!sem->is_static) free(sem);
}

typedef struct {
  void (*entry)(void*);
  void* user;
  int role;
} test_start_t;
static void* test_task_entry(void* user) {
  test_start_t start = *(test_start_t*)user;
  free(user);
  test_is_worker = start.role;
  start.entry(start.user);
  abort();
}
static int xTaskCreate(void (*entry)(void*), const char* name, uint32_t stack,
                       void* user, unsigned priority, TaskHandle_t* out) {
  assert((strcmp(name, "h2_tls") == 0 || strcmp(name, "h2_net_reap") == 0 ||
          strcmp(name, "h2_tls_free") == 0) &&
         stack == 8192u && priority == 1u);
  ++test_dynamic_task_calls;
  test_sdk_gate(8);
  if (test_fail_task) {
    --test_fail_task;
    return 0;
  }
  test_start_t* start = malloc(sizeof(*start));
  pthread_t* thread = malloc(sizeof(*thread));
  assert(start && thread);
  *start = (test_start_t){entry, user,
                          strcmp(name, "h2_tls") == 0        ? 1
                          : strcmp(name, "h2_net_reap") == 0 ? 2
                                                             : 3};
  assert(pthread_create(thread, NULL, test_task_entry, start) == 0);
  assert(pthread_detach(*thread) == 0);
  *out = thread;
  ++test_task_count;
  return pdPASS;
}

static void* test_static_task_entry(void* context) {
  StaticTask_t* task = context;
  test_is_worker = task->role;
  task->entry(task->user);
  abort();
}
static TaskHandle_t xTaskCreateStaticPinnedToCore(
    TaskFunction_t entry, const char* name, uint32_t stack, void* user,
    unsigned priority, StackType_t* buffer, StaticTask_t* task, int core) {
  assert(stack == 8192u && priority == 1u && core == tskNO_AFFINITY);
  assert(buffer != NULL && ((uintptr_t)buffer % portBYTE_ALIGNMENT) == 0u &&
         task != NULL);
  assert(pthread_mutex_lock(&test_lock) == 0);
  ++test_static_task_attempts;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_sdk_gate(16);
  if (test_static_delay_ms) vTaskDelay(test_static_delay_ms);
  if (test_fail_task) {
    if (test_fail_task > 0) --test_fail_task;
    return NULL;
  }
  task->entry = entry;
  task->user = user;
  task->role = strcmp(name, "h2_tls") == 0        ? 1
               : strcmp(name, "h2_net_reap") == 0 ? 2
                                                  : 3;
  assert(pthread_create(&task->thread, NULL, test_static_task_entry, task) ==
         0);
  assert(pthread_detach(task->thread) == 0);
  ++test_task_count;
  return &task->thread;
}

static void* test_allocate(size_t length) {
  test_sdk_gate(7);
  assert(pthread_mutex_lock(&test_lock) == 0);
  int fail = test_alloc_fail || test_alloc_persistent;
  test_alloc_fail = 0;
  if (!fail) ++test_allocations;
  if (test_is_worker == 1)
    ++test_worker_allocations;
  else
    ++test_caller_allocations;
  if (length > test_max_allocation) test_max_allocation = length;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return fail ? NULL : malloc(length);
}
static void test_deallocate(void* memory) {
  if (test_is_worker == 1) test_sdk_gate(11);
  assert(memory != NULL);
  assert(pthread_mutex_lock(&test_lock) == 0);
  --test_allocations;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  free(memory);
}
#define h2_esp_platform_default_allocator() NULL
#define h2_pal_mem_alloc(allocator, length) test_allocate(length)
#define h2_pal_mem_free(allocator, memory) test_deallocate(memory)
#define h2_esp_platform_crypto_api() NULL
static int h2_pal_crypto_random(void* api, void* out, size_t length) {
  (void)api;
  memset(out, 0xa5, length);
  return H2_PAL_OK;
}
#define h2_esp_platform_time_api() NULL
static int h2_pal_time_get_wall_ms(void* api, uint64_t* out) {
  (void)api;
  *out = 1794268800000ULL;
  return H2_PAL_OK;
}
static int h2_esp_net_tls_verify_certificate(
    const h2_esp_net_tls_verify_t* verify, mbedtls_x509_crt* cert, int depth,
    uint32_t* flags, const mbedtls_x509_time* now) {
  (void)verify;
  (void)cert;
  (void)depth;
  (void)flags;
  (void)now;
  return 0;
}

static void test_sdk_gate(int stage) {
  assert(pthread_mutex_lock(&test_lock) == 0);
  /* Select exactly one primary stall even when helpers win the first native
   * close claims. Helpers proceed once the primary reaches destruction. */
  while (stage == 6 && test_gate_stage == 6 && test_gate_primary_only &&
         test_is_worker == 1 && test_gate_entered == 0)
    assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
  if (test_gate_stage == stage &&
      (!test_gate_primary_only || test_is_worker == 3)) {
    ++test_gate_entered;
    assert(pthread_cond_broadcast(&test_changed) == 0);
    while (!test_gate_release)
      assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
  }
  assert(pthread_mutex_unlock(&test_lock) == 0);
}
static void mbedtls_ssl_init(mbedtls_ssl_context* ssl) {
  memset(ssl, 0, sizeof(*ssl));
  ssl->self = ssl;
  assert(pthread_mutex_lock(&test_lock) == 0);
  ++test_contexts;
  assert(pthread_mutex_unlock(&test_lock) == 0);
}
static void mbedtls_ssl_config_init(mbedtls_ssl_config* config) {
  memset(config, 0, sizeof(*config));
  config->self = config;
}
static void mbedtls_x509_crt_init(mbedtls_x509_crt* ca) { ca->self = ca; }
static int mbedtls_ssl_config_defaults(mbedtls_ssl_config* config, int a, int b,
                                       int c) {
  (void)config;
  (void)a;
  (void)b;
  (void)c;
  test_sdk_gate(1);
  return 0;
}
static int psa_crypto_init(void) { return PSA_SUCCESS; }
static void mbedtls_ssl_conf_rng(mbedtls_ssl_config* config,
                                 int (*rng)(void*, unsigned char*, size_t),
                                 void* user) {
  (void)config;
  unsigned char byte;
  assert(rng(user, &byte, 1u) == 0);
}
static void mbedtls_ssl_conf_authmode(mbedtls_ssl_config* config, int mode) {
  (void)config;
  (void)mode;
}
static int esp_crt_bundle_attach(mbedtls_ssl_config* config) {
  (void)config;
  test_sdk_gate(4);
  return ESP_OK;
}
static int mbedtls_x509_crt_parse(mbedtls_x509_crt* ca,
                                  const unsigned char* pem, size_t length) {
  assert(ca->self == ca);
  test_sdk_gate(2);
  assert(length == test_expected_ca_length + 1u && pem[length - 1u] == 0 &&
         memcmp(pem, "ROOT", 4u) == 0);
  for (size_t i = 4u; i + 1u < length; ++i) assert(pem[i] == ' ');
  assert(pthread_mutex_lock(&test_lock) == 0);
  ++test_ca_checks;
  int fail = test_parse_fail;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return fail ? -1 : 0;
}
static void mbedtls_ssl_conf_ca_chain(mbedtls_ssl_config* config,
                                      mbedtls_x509_crt* ca, void* crl) {
  (void)crl;
  config->ca = ca;
}
static void mbedtls_ssl_conf_verify(mbedtls_ssl_config* config,
                                    test_verify_fn fn, void* user) {
  config->f_vrfy = fn;
  config->p_vrfy = user;
}
static int mbedtls_ssl_conf_alpn_protocols(mbedtls_ssl_config* config,
                                           const char** alpn) {
  assert(strcmp(alpn[0], "h2") == 0 && alpn[1] == NULL);
  assert(pthread_mutex_lock(&test_lock) == 0);
  ++test_alpn_checks;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  config->alpn = alpn;
  return 0;
}
static int mbedtls_ssl_setup(mbedtls_ssl_context* ssl,
                             mbedtls_ssl_config* config) {
  assert(ssl->self == ssl && config->self == config);
  ssl->config = config;
  test_sdk_gate(3);
  return 0;
}
static int mbedtls_ssl_set_hostname(mbedtls_ssl_context* ssl,
                                    const char* name) {
  (void)ssl;
  assert(strcmp(name, "h2.example") == 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  ++test_sni_checks;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return 0;
}
static void mbedtls_ssl_set_bio(mbedtls_ssl_context* ssl, void* user,
                                int (*send_fn)(void*, const unsigned char*,
                                               size_t),
                                int (*recv_fn)(void*, unsigned char*, size_t),
                                void* timeout_fn) {
  (void)timeout_fn;
  assert(ssl->self == ssl);
  ssl->bio = user;
  ssl->send_fn = send_fn;
  ssl->recv_fn = recv_fn;
}
static void* test_registry_blocker(void* unused) {
  (void)unused;
  assert(xSemaphoreTake(test_registry, portMAX_DELAY));
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_publish_held = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  while (!test_publish_release)
    assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(xSemaphoreGive(test_registry));
  return NULL;
}
static int mbedtls_ssl_handshake(mbedtls_ssl_context* ssl) {
  assert(ssl->self == ssl && ssl->config->self == ssl->config);
  test_sdk_gate(5);
  if (ssl->phase == 0) {
    int rc = ssl->send_fn(ssl->bio, (const unsigned char*)"HELLO", 5u);
    if (rc < 0) return rc;
    assert(rc == 5);
    ++ssl->phase;
  }
  if (ssl->phase == 1) {
    unsigned char input[5];
    int rc = ssl->recv_fn(ssl->bio, input, sizeof(input));
    if (rc < 0) return rc;
    if (rc == 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    assert(rc == 5 && memcmp(input, "WORLD", 5u) == 0);
    ++ssl->phase;
  }
  assert(pthread_mutex_lock(&test_lock) == 0);
  if (test_publish_block) {
    test_publish_block = 0;
    assert(pthread_create(&test_publish_thread, NULL, test_registry_blocker,
                          NULL) == 0);
    while (!test_publish_held)
      assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
  }
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return 0;
}
static uint32_t mbedtls_ssl_get_verify_result(mbedtls_ssl_context* ssl) {
  (void)ssl;
  return 0;
}
static void mbedtls_ssl_free(mbedtls_ssl_context* ssl) {
  test_sdk_gate(6);
  assert(ssl->self == ssl);
  assert(pthread_mutex_lock(&test_lock) == 0);
  --test_contexts;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  ssl->self = NULL;
}
static void mbedtls_ssl_config_free(mbedtls_ssl_config* config) {
  assert(config->self == config);
}
static void mbedtls_x509_crt_free(mbedtls_x509_crt* ca) {
  assert(ca->self == ca);
}

static int test_select(int count, fd_set* reads, fd_set* writes, fd_set* errors,
                       struct timeval* timeout) {
  assert(pthread_mutex_lock(&test_lock) == 0);
  if (test_write_wait_fd >= 0 && writes != NULL &&
      FD_ISSET(test_write_wait_fd, writes)) {
    test_write_wait_entered = 1;
    assert(pthread_cond_broadcast(&test_changed) == 0);
  }
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return select(count, reads, writes, errors, timeout);
}
#define select test_select
static int test_send(int fd, const void* bytes, size_t length, int flags) {
  assert(!test_is_worker);
  assert(pthread_mutex_lock(&test_lock) == 0);
  ++test_io_calls;
  test_probe_send_fd = fd;
  test_probe_send_flags = flags;
  test_probe_send_start = esp_timer_get_time();
  test_probe_send_end = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  if (fd == test_send_prefix_fd && length > test_send_prefix_length)
    length = test_send_prefix_length;
  /* Match lwIP MSG_DONTWAIT even on Darwin's blocking AF_UNIX pressure path.
   * The ordinary flags=0 syscall remains a real blocked native send. */
  int original_flags = -1;
  if (fd == test_emulate_dontwait_fd) {
    assert(pthread_mutex_lock(&test_lock) == 0);
    test_plain_send_entered = 1;
    assert(pthread_cond_broadcast(&test_changed) == 0);
    assert(pthread_mutex_unlock(&test_lock) == 0);
    if (flags & MSG_DONTWAIT) {
      original_flags = fcntl(fd, F_GETFL, 0);
      assert(original_flags >= 0);
      assert(fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) == 0);
    }
  }
  int result = (int)send(fd, bytes, length, flags);
  int error = errno;
  if (original_flags >= 0) assert(fcntl(fd, F_SETFL, original_flags) == 0);
  errno = error;
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_probe_send_result = result;
  test_probe_send_end = esp_timer_get_time();
  assert(pthread_mutex_unlock(&test_lock) == 0);
  errno = error;
  if (result >= 0) test_sdk_gate(12);
  return result;
}
static int test_recv(int fd, void* bytes, size_t length, int flags) {
  assert(!test_is_worker);
  test_sdk_gate(10);
  assert(pthread_mutex_lock(&test_lock) == 0);
  if (fd == test_hold_recv_fd) {
    test_recv_held = 1;
    assert(pthread_cond_broadcast(&test_changed) == 0);
    while (!test_recv_release)
      assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
  }
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  ++test_io_calls;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_probe_recv_fd = fd;
  test_probe_recv_flags = flags;
  test_probe_recv_start = esp_timer_get_time();
  test_probe_recv_end = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  int result = (int)recv(fd, bytes, length, flags);
  int error = errno;
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_probe_recv_result = result;
  test_probe_recv_end = esp_timer_get_time();
  assert(pthread_mutex_unlock(&test_lock) == 0);
  errno = error;
  if (result >= 0) test_sdk_gate(13);
  return result;
}
/* AF_UNIX transports stand in for lwIP TCP. Darwin rejects SO_LINGER changes
 * on a disconnected AF_UNIX socket, while lwIP updates conn->linger even after
 * shutdown. Model that SDK field here; real AF_INET regressions use the kernel
 * option and verify delivered bytes followed by graceful EOF. */
static struct linger test_unix_linger[FD_SETSIZE];
static int test_is_unix_socket(int fd) {
  struct sockaddr_storage address;
  socklen_t length = sizeof(address);
  return getsockname(fd, (struct sockaddr*)&address, &length) == 0 &&
         address.ss_family == AF_UNIX;
}
static int test_setsockopt(int fd, int level, int option, const void* value,
                           socklen_t length) {
  if (level == SOL_SOCKET && option == SO_LINGER &&
      length == sizeof(struct linger) &&
      ((const struct linger*)value)->l_onoff &&
      ((const struct linger*)value)->l_linger == 0) {
    assert(pthread_mutex_lock(&test_lock) == 0);
    int fail = test_fail_abortive_linger > 0;
    if (fail) --test_fail_abortive_linger;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    if (fail) {
      errno = ENOMEM;
      return -1;
    }
  }
  if (level == SOL_SOCKET && option == SO_LINGER &&
      length == sizeof(struct linger) &&
      !((const struct linger*)value)->l_onoff) {
    assert(pthread_mutex_lock(&test_lock) == 0);
    ++test_normal_linger_attempts;
    int fail = test_fail_normal_linger > 0;
    if (fail) {
      --test_fail_normal_linger;
      uint64_t now = (uint64_t)esp_timer_get_time() / 1000u;
      if (test_linger_first_failure_ms == 0u)
        test_linger_first_failure_ms = now;
      test_linger_last_failure_ms = now;
    }
    assert(pthread_mutex_unlock(&test_lock) == 0);
    if (fail) {
      errno = ENOMEM;
      return -1;
    }
  }
  if (level == SOL_SOCKET && option == SO_LINGER && test_is_unix_socket(fd)) {
    assert(fd >= 0 && fd < FD_SETSIZE && length == sizeof(struct linger));
    assert(pthread_mutex_lock(&test_lock) == 0);
    test_unix_linger[fd] = *(const struct linger*)value;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    return 0;
  }
  return setsockopt(fd, level, option, value, length);
}
static void test_close_profile(int fd, int closing) {
  int type = 0;
  socklen_t length = sizeof(type);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 &&
      type == SOCK_STREAM) {
    struct linger profile;
    length = sizeof(profile);
    if (test_is_unix_socket(fd)) {
      assert(fd >= 0 && fd < FD_SETSIZE);
      assert(pthread_mutex_lock(&test_lock) == 0);
      profile = test_unix_linger[fd];
      assert(pthread_mutex_unlock(&test_lock) == 0);
    } else
      assert(getsockopt(fd, SOL_SOCKET, SO_LINGER, &profile, &length) == 0);
    assert(pthread_mutex_lock(&test_lock) == 0);
    int normal = fd == test_expect_normal_fd;
    int fin_wait = test_simulate_fin_wait &&
                   (test_fin_wait_fd < 0 || test_fin_wait_fd == fd);
    assert(pthread_mutex_unlock(&test_lock) == 0);
    if (normal) assert(profile.l_onoff == 0);
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);
    int connected = getpeername(fd, (struct sockaddr*)&peer, &peer_length) == 0;
    if (closing && connected && fin_wait &&
        !(profile.l_onoff && profile.l_linger == 0))
      test_sdk_gate(15);
  }
}
static int test_close(int fd) {
  /* Idle setup actors may close exclusively owned retired native records. */
  test_close_profile(fd, 1);
  assert(pthread_mutex_lock(&test_lock) == 0);
  ++test_closed;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return close(fd);
}
static int test_shutdown(int fd, int how) {
#if defined(__APPLE__)
  /* Darwin AF_UNIX SHUT_RD clears SO_RCVTIMEO. The captured racing receive
   * then remained in __recvfrom with a zero timeout after shutdown completed.
   * This host transport is not lwIP: keep its finite receive timeout intact
   * and exercise production lease polling. AF_INET and Linux use native
   * shutdown; the real SDK's rx-only shutdown path is checked separately. */
  if (how == SHUT_RD && test_is_unix_socket(fd)) return 0;
#endif
  if (how != SHUT_RD) test_close_profile(fd, 0);
  int listening = 0;
  socklen_t length = sizeof(listening);
  int type = 0;
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 &&
      type == SOCK_DGRAM) {
    errno = EOPNOTSUPP;
    return -1;
  }
  length = sizeof(listening);
  if (test_listener_shutdown_noop &&
      getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &length) == 0 &&
      listening)
    return 0;
  return shutdown(fd, how);
}
static int test_connect(int fd, const struct sockaddr* address,
                        socklen_t length) {
  assert(pthread_mutex_lock(&test_lock) == 0);
  ++test_native_connect_calls;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return connect(fd, address, length);
}
static int test_accept(int fd, struct sockaddr* address, socklen_t* length) {
  assert(pthread_mutex_lock(&test_lock) == 0);
  ++test_native_accept_calls;
  int would_block = test_accept_would_block;
  if (would_block) --test_accept_would_block;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  if (would_block) {
    errno = EAGAIN;
    return -1;
  }
  int accepted = accept(fd, address, length);
  if (accepted >= 0) {
    assert(pthread_mutex_lock(&test_lock) == 0);
    test_last_accepted_fd = accepted;
    if (test_hold_accept) {
      test_accept_entered = 1;
      assert(pthread_cond_broadcast(&test_changed) == 0);
      while (!test_accept_release)
        assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
    }
    assert(pthread_mutex_unlock(&test_lock) == 0);
  }
  return accepted;
}
static int test_fcntl(int fd, int command, ...) {
  int value = 0;
  if (command == F_SETFL) {
    va_list args;
    va_start(args, command);
    value = va_arg(args, int);
    va_end(args);
  }
  assert(pthread_mutex_lock(&test_lock) == 0);
  int fail = fd == test_last_accepted_fd &&
             command == test_fail_accepted_fcntl_command;
  if (fail) test_fail_accepted_fcntl_command = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  if (fail) {
    errno = ENOMEM;
    return -1;
  }
  return fcntl(fd, command, value);
}
#define fcntl test_fcntl
#define connect test_connect
#define accept test_accept
#define send test_send
#define recv test_recv
#define close test_close
#define shutdown test_shutdown
#define setsockopt test_setsockopt

typedef int esp_err_t;
static esp_err_t esp_netif_tcpip_exec(esp_err_t (*callback)(void*),
                                      void* user) {
  return callback(user);
}
#define netif_index_to_name(index, name) if_indextoname(index, name)
static const h2_pal_netif_api_t* h2_esp_platform_netif_api(void) {
  return NULL;
}
static int mbedtls_ssl_write(mbedtls_ssl_context* ssl,
                             const unsigned char* data, size_t length) {
  assert(ssl->self == ssl);
  if (test_ssl_write_pending) {
    assert(data == test_ssl_write_pointer && length == test_ssl_write_length);
    ++test_ssl_write_retries;
  }
  int result = ssl->send_fn(ssl->bio, data, length);
  test_ssl_write_pending = result == MBEDTLS_ERR_SSL_WANT_WRITE ||
                           result == MBEDTLS_ERR_SSL_WANT_READ;
  if (test_ssl_write_pending) {
    test_ssl_write_pointer = data;
    test_ssl_write_length = length;
  }
  return result;
}
static int mbedtls_ssl_read(mbedtls_ssl_context* ssl, unsigned char* data,
                            size_t length) {
  assert(ssl->self == ssl);
  return ssl->recv_fn(ssl->bio, data, length);
}
static void* test_memcpy(void* dest, const void* source, size_t length) {
  if (!test_is_worker && length == 2048u) {
    assert(pthread_mutex_lock(&test_lock) == 0);
    if (test_copy_gate) {
      test_copy_entered = 1;
      assert(pthread_cond_broadcast(&test_changed) == 0);
      while (!test_copy_release)
        assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
    }
    assert(pthread_mutex_unlock(&test_lock) == 0);
  }
  return memcpy(dest, source, length);
}
#undef memcpy
#define memcpy test_memcpy

#endif
