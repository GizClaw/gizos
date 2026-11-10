#ifndef H2_ESP_PLATFORM_NET_TLS_BOUNDED_H
#define H2_ESP_PLATFORM_NET_TLS_BOUNDED_H

#include "h2_esp_platform_net_lifetime.h"

#define H2_ESP_NET_TLS_WORKER_MAX 2u
#define H2_ESP_NET_TLS_WORKER_STACK_BYTES 8192u
#define H2_ESP_NET_TLS_SERVER_NAME_MAX 253u
#define H2_ESP_NET_TLS_MAILBOX_BYTES 2048u

typedef struct esp_net_tls_worker esp_net_tls_worker_t;
typedef enum esp_net_tls_job_decision {
  ESP_NET_TLS_JOB_WAITING = 0,
  ESP_NET_TLS_JOB_ACCEPTED,
  ESP_NET_TLS_JOB_ABANDONED,
} esp_net_tls_job_decision_t;
typedef enum esp_net_tls_job_request {
  ESP_NET_TLS_JOB_IDLE = 0,
  ESP_NET_TLS_JOB_SEND,
  ESP_NET_TLS_JOB_RECV,
  ESP_NET_TLS_JOB_REPLIED,
} esp_net_tls_job_request_t;
typedef struct esp_net_tls_job {
  esp_net_tls_worker_t* worker;
  SemaphoreHandle_t caller_wake;
  size_t slot_index;
  uint32_t generation;
  uint64_t deadline_ms;
  h2_pal_net_tls_config_t config;
  char server_name[H2_ESP_NET_TLS_SERVER_NAME_MAX + 1u];
  char alpn[H2_ESP_NET_TLS_ALPN_LEN];
  /* Permanent control/wake objects. Transitions transfer exclusive access to
   * the buffer; only the caller's final decision permits worker reclamation. */
  int submitted, buffer_ready, input_ready, completed;
  esp_net_tls_job_decision_t decision;
  esp_net_tls_job_request_t request;
  h2_pal_result_t result;
  uint8_t* ca;
  size_t io_length;
  int io_result;
  uint8_t io[H2_ESP_NET_TLS_MAILBOX_BYTES];
} esp_net_tls_job_t;
struct esp_net_tls_worker {
  SemaphoreHandle_t wake;
  TaskHandle_t task;
  esp_net_tls_job_t* job;
};
static esp_net_tls_worker_t s_esp_tls_workers[H2_ESP_NET_TLS_WORKER_MAX];
static esp_net_tls_job_t s_esp_tls_jobs[H2_ESP_NET_TLS_WORKER_MAX];
static int s_esp_tls_pool_ready, s_esp_tls_legacy_ready;

static int esp_net_tls_available(void) {
  portENTER_CRITICAL(&s_esp_net_mux);
  int ready = s_esp_tls_legacy_ready;
  portEXIT_CRITICAL(&s_esp_net_mux);
  return ready;
}

static void esp_net_tls_jobs_wake(void) {
  for (size_t i = 0u; i < H2_ESP_NET_TLS_WORKER_MAX; ++i) {
    portENTER_CRITICAL(&s_esp_net_mux);
    SemaphoreHandle_t worker = s_esp_tls_workers[i].wake;
    SemaphoreHandle_t caller = s_esp_tls_jobs[i].caller_wake;
    portEXIT_CRITICAL(&s_esp_net_mux);
    if (worker != NULL) (void)xSemaphoreGive(worker);
    if (caller != NULL) (void)xSemaphoreGive(caller);
  }
}

static esp_net_tls_job_decision_t esp_net_tls_job_decision(
    esp_net_tls_job_t* job) {
  portENTER_CRITICAL(&s_esp_net_mux);
  esp_net_tls_job_decision_t decision = job->decision;
  portEXIT_CRITICAL(&s_esp_net_mux);
  return decision;
}

static void esp_net_tls_job_decide(esp_net_tls_job_t* job,
                                   esp_net_tls_job_decision_t decision) {
  esp_net_tls_worker_t* worker = job->worker;
  portENTER_CRITICAL(&s_esp_net_mux);
  job->decision = decision;
  portEXIT_CRITICAL(&s_esp_net_mux);
  /* The last job access. This semaphore is permanent, even across reuse. */
  (void)xSemaphoreGive(worker->wake);
}

static int esp_net_tls_job_io(esp_net_tls_job_t* job,
                              esp_net_tls_job_request_t request,
                              const unsigned char* send_data,
                              unsigned char* recv_data, size_t length) {
  if (length > sizeof(job->io)) length = sizeof(job->io);
  if (request == ESP_NET_TLS_JOB_SEND) memcpy(job->io, send_data, length);
  job->io_length = length;
  portENTER_CRITICAL(&s_esp_net_mux);
  job->request = request;
  portEXIT_CRITICAL(&s_esp_net_mux);
  (void)xSemaphoreGive(job->caller_wake);
  for (;;) {
    portENTER_CRITICAL(&s_esp_net_mux);
    int abandoned = job->decision == ESP_NET_TLS_JOB_ABANDONED;
    int replied = job->request == ESP_NET_TLS_JOB_REPLIED;
    portEXIT_CRITICAL(&s_esp_net_mux);
    if (abandoned) break;
    if (replied) {
      int result = job->io_result;
      if (result > 0 && request == ESP_NET_TLS_JOB_RECV)
        memcpy(recv_data, job->io, (size_t)result);
      return result;
    }
    uint32_t remaining = esp_net_timeout_remaining_ms(job->deadline_ms);
    if (remaining == 0u) break;
    (void)xSemaphoreTake(job->worker->wake,
                         esp_net_tls_timeout_ticks(remaining));
  }
  return request == ESP_NET_TLS_JOB_SEND ? MBEDTLS_ERR_NET_SEND_FAILED
                                         : MBEDTLS_ERR_NET_RECV_FAILED;
}
static int esp_net_tls_job_send(void* user, const unsigned char* data,
                                size_t length) {
  return esp_net_tls_job_io(user, ESP_NET_TLS_JOB_SEND, data, NULL, length);
}
static int esp_net_tls_job_recv(void* user, unsigned char* data,
                                size_t length) {
  return esp_net_tls_job_io(user, ESP_NET_TLS_JOB_RECV, NULL, data, length);
}
static h2_pal_result_t esp_net_tls_job_handshake(esp_net_tls_job_t* job,
                                                 esp_net_tls_socket_t* slot) {
  mbedtls_ssl_set_bio(&slot->ssl, job, esp_net_tls_job_send,
                      esp_net_tls_job_recv, NULL);
  for (;;) {
    if (esp_net_tls_job_decision(job) != ESP_NET_TLS_JOB_WAITING ||
        esp_net_timeout_remaining_ms(job->deadline_ms) == 0u)
      return H2_PAL_ERR_TIMEOUT;
    int result = mbedtls_ssl_handshake(&slot->ssl);
    if (result == 0)
      return job->config.verify == H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY ||
                     mbedtls_ssl_get_verify_result(&slot->ssl) == 0u
                 ? H2_PAL_OK
                 : H2_PAL_ERR_TLS_VERIFY;
    if (result != MBEDTLS_ERR_SSL_WANT_READ &&
        result != MBEDTLS_ERR_SSL_WANT_WRITE)
      return result == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
                 ? H2_PAL_ERR_TLS_VERIFY
                 : H2_PAL_ERR_IO;
    (void)xSemaphoreTake(job->worker->wake, 1u);
  }
}

static void esp_net_tls_job_worker(void* user) {
  esp_net_tls_worker_t* worker = user;
  for (;;) {
    (void)xSemaphoreTake(worker->wake, portMAX_DELAY);
    portENTER_CRITICAL(&s_esp_net_mux);
    esp_net_tls_job_t* job = worker->job;
    int submitted = job != NULL && job->submitted;
    portEXIT_CRITICAL(&s_esp_net_mux);
    if (!submitted) continue;
    size_t index = job->slot_index;
    esp_net_tls_socket_t* slot = &s_esp_tls_sockets[index];
    h2_pal_result_t rc = H2_PAL_OK;
    int initialized = 0;
    /* No fixed CA byte limit: one owned NUL-terminated input allocation,
     * naturally limited by available provider heap. Allocation can stall
     * indefinitely without holding a caller fd, pointer or control resource. */
    if (esp_net_tls_job_decision(job) == ESP_NET_TLS_JOB_WAITING &&
        job->config.root_ca_pem_len != 0u) {
      job->ca = h2_pal_mem_alloc(h2_esp_platform_default_allocator(),
                                 job->config.root_ca_pem_len + 1u);
      if (job->ca == NULL) rc = H2_PAL_ERR_NO_MEMORY;
    }
    portENTER_CRITICAL(&s_esp_net_mux);
    job->buffer_ready = 1;
    portEXIT_CRITICAL(&s_esp_net_mux);
    (void)xSemaphoreGive(job->caller_wake);
    if (rc == H2_PAL_OK) {
      for (;;) {
        portENTER_CRITICAL(&s_esp_net_mux);
        int ready = job->input_ready;
        int abandoned = job->decision == ESP_NET_TLS_JOB_ABANDONED;
        portEXIT_CRITICAL(&s_esp_net_mux);
        if (abandoned) {
          rc = H2_PAL_ERR_TIMEOUT;
          break;
        }
        if (ready) break;
        (void)xSemaphoreTake(worker->wake, portMAX_DELAY);
      }
    }
    if (rc == H2_PAL_OK) {
      job->config.root_ca_pem = job->ca;
      initialized = 1;
      rc = esp_net_tls_setup(slot, &job->config, job->ca);
      if (rc == H2_PAL_OK) rc = esp_net_tls_job_handshake(job, slot);
    }
    if (esp_net_timeout_remaining_ms(job->deadline_ms) == 0u)
      rc = H2_PAL_ERR_TIMEOUT;
    portENTER_CRITICAL(&s_esp_net_mux);
    job->result = rc;
    job->completed = 1;
    portEXIT_CRITICAL(&s_esp_net_mux);
    (void)xSemaphoreGive(job->caller_wake);
    esp_net_tls_job_decision_t decision;
    while ((decision = esp_net_tls_job_decision(job)) ==
           ESP_NET_TLS_JOB_WAITING)
      (void)xSemaphoreTake(worker->wake, portMAX_DELAY);
    if (decision == ESP_NET_TLS_JOB_ABANDONED && initialized) {
      mbedtls_ssl_free(&slot->ssl);
      mbedtls_ssl_config_free(&slot->config);
      mbedtls_x509_crt_free(&slot->ca);
    }
    if (job->ca != NULL)
      h2_pal_mem_free(h2_esp_platform_default_allocator(), job->ca);
    (void)xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY);
    if (decision == ESP_NET_TLS_JOB_ABANDONED) {
      memset(slot, 0, sizeof(*slot));
      slot->fd = -1;
      ++s_esp_tls_sync->generations[index];
    }
    portENTER_CRITICAL(&s_esp_net_mux);
    worker->job = NULL;
    portEXIT_CRITICAL(&s_esp_net_mux);
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
  }
}

/* Only called during provider initialization, never by the bounded entry.
 * Keep partial permanent resources for a later initialization retry. */
static int esp_net_tls_bounded_init(void) {
  if (s_esp_net_reaper_wake == NULL) {
    SemaphoreHandle_t wake = xSemaphoreCreateBinary();
    portENTER_CRITICAL(&s_esp_net_mux);
    s_esp_net_reaper_wake = wake;
    portEXIT_CRITICAL(&s_esp_net_mux);
  }
  if (s_esp_net_reaper_task == NULL && s_esp_net_reaper_wake != NULL) {
    TaskHandle_t task = NULL;
    if (xTaskCreate(esp_net_socket_reaper, "h2_net_reap",
                    H2_ESP_NET_TLS_WORKER_STACK_BYTES, NULL,
                    tskIDLE_PRIORITY + 1u, &task) == pdPASS) {
      portENTER_CRITICAL(&s_esp_net_mux);
      s_esp_net_reaper_task = task;
      portEXIT_CRITICAL(&s_esp_net_mux);
      esp_net_socket_kick();
    }
  }
  int ready = s_esp_net_reaper_task != NULL && s_esp_tls_sync != NULL;
  portENTER_CRITICAL(&s_esp_net_mux);
  s_esp_tls_legacy_ready = ready;
  portEXIT_CRITICAL(&s_esp_net_mux);
  if (!ready) return 0;
  for (size_t i = 0u; i < H2_ESP_NET_TLS_WORKER_MAX; ++i) {
    esp_net_tls_worker_t* worker = &s_esp_tls_workers[i];
    esp_net_tls_job_t* job = &s_esp_tls_jobs[i];
    if (worker->wake == NULL) {
      SemaphoreHandle_t wake = xSemaphoreCreateBinary();
      portENTER_CRITICAL(&s_esp_net_mux);
      worker->wake = wake;
      portEXIT_CRITICAL(&s_esp_net_mux);
    }
    if (job->caller_wake == NULL) {
      SemaphoreHandle_t wake = xSemaphoreCreateBinary();
      portENTER_CRITICAL(&s_esp_net_mux);
      job->caller_wake = wake;
      portEXIT_CRITICAL(&s_esp_net_mux);
    }
    if (worker->task == NULL && worker->wake != NULL &&
        job->caller_wake != NULL) {
      TaskHandle_t task = NULL;
      if (xTaskCreate(esp_net_tls_job_worker, "h2_tls",
                      H2_ESP_NET_TLS_WORKER_STACK_BYTES, worker,
                      tskIDLE_PRIORITY + 1u, &task) == pdPASS)
        worker->task = task;
    }
    ready &= worker->task != NULL;
  }
  portENTER_CRITICAL(&s_esp_net_mux);
  s_esp_tls_pool_ready = ready;
  portEXIT_CRITICAL(&s_esp_net_mux);
  return ready;
}

static int esp_net_tls_job_service(esp_net_tls_job_t* job, int fd,
                                   esp_net_tls_job_request_t request) {
  int result = request == ESP_NET_TLS_JOB_SEND
                   ? send(fd, job->io, job->io_length, MSG_DONTWAIT)
                   : recv(fd, job->io, job->io_length, MSG_DONTWAIT);
  if (result < 0 &&
      (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
    uint32_t remaining = esp_net_timeout_remaining_ms(job->deadline_ms);
    return remaining == 0u
               ? H2_PAL_ERR_TIMEOUT
               : h2_esp_net_wait_fd(fd, request == ESP_NET_TLS_JOB_SEND,
                                    remaining);
  }
  job->io_result = result >= 0 ? result
                   : request == ESP_NET_TLS_JOB_SEND
                       ? MBEDTLS_ERR_NET_SEND_FAILED
                       : MBEDTLS_ERR_NET_RECV_FAILED;
  portENTER_CRITICAL(&s_esp_net_mux);
  job->request = ESP_NET_TLS_JOB_REPLIED;
  portEXIT_CRITICAL(&s_esp_net_mux);
  (void)xSemaphoreGive(job->worker->wake);
  return H2_PAL_OK;
}

static h2_pal_result_t esp_net_tls_wrap_bounded(
    void* user, h2_pal_net_socket_t fd, const h2_pal_net_tls_config_t* config,
    uint32_t timeout_ms, h2_pal_net_socket_t* out) {
  (void)user;
  uint64_t deadline = esp_net_now_ms() + timeout_ms;
  if (out != NULL) *out = -1;
  if (fd < 0 || config == NULL || out == NULL || timeout_ms == 0u ||
      config->server_name == NULL || config->server_name[0] == '\0' ||
      (config->root_ca_pem == NULL && config->root_ca_pem_len != 0u) ||
      config->root_ca_pem_len == SIZE_MAX ||
      (config->verify != H2_PAL_NET_TLS_VERIFY_DEFAULT &&
       config->verify != H2_PAL_NET_TLS_VERIFY_REQUIRED &&
       config->verify != H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY))
    return H2_PAL_ERR_INVALID_ARG;
  portENTER_CRITICAL(&s_esp_net_mux);
  int ready = s_esp_tls_pool_ready;
  portEXIT_CRITICAL(&s_esp_net_mux);
  if (!ready) return H2_PAL_ERR_UNAVAILABLE;
  size_t name_length =
      strnlen(config->server_name, H2_ESP_NET_TLS_SERVER_NAME_MAX + 1u);
  size_t alpn_length = config->alpn == NULL
                           ? 0u
                           : strnlen(config->alpn, H2_ESP_NET_TLS_ALPN_LEN);
  if (name_length > H2_ESP_NET_TLS_SERVER_NAME_MAX ||
      alpn_length >= H2_ESP_NET_TLS_ALPN_LEN)
    return H2_PAL_ERR_NO_SPACE;
  esp_net_socket_lease_t lease;
  int rc = esp_net_socket_pin(fd, SOCK_STREAM, 1, &lease);
  if (rc != H2_PAL_OK) return rc;
  esp_net_tls_job_t* job = NULL;
  rc = esp_net_tls_take_mutex(s_esp_tls_sync->registry_mutex, 1, timeout_ms,
                              deadline);
  if (rc == H2_PAL_OK) {
    int index = esp_net_tls_find_free();
    portENTER_CRITICAL(&s_esp_net_mux);
    if (index >= 0) {
      for (size_t i = 0u; i < H2_ESP_NET_TLS_WORKER_MAX; ++i) {
        if (s_esp_tls_workers[i].job == NULL) {
          job = &s_esp_tls_jobs[i];
          SemaphoreHandle_t wake = job->caller_wake;
          memset(job, 0, sizeof(*job));
          job->caller_wake = wake;
          job->worker = &s_esp_tls_workers[i];
          job->worker->job = job;
          job->slot_index = (size_t)index;
          break;
        }
      }
    }
    portEXIT_CRITICAL(&s_esp_net_mux);
    if (job == NULL)
      rc = H2_PAL_ERR_NO_SPACE;
    else {
      esp_net_tls_socket_t* slot = &s_esp_tls_sockets[index];
      memset(slot, 0, sizeof(*slot));
      slot->state = ESP_NET_TLS_SOCKET_CONFIGURING;
      slot->fd = -1;
      job->generation = ++s_esp_tls_sync->generations[index];
    }
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
  }
  if (job != NULL) {
    job->deadline_ms = deadline;
    job->config.verify = config->verify;
    job->config.root_ca_pem_len = config->root_ca_pem_len;
    memcpy(job->server_name, config->server_name, name_length + 1u);
    job->config.server_name = job->server_name;
    if (config->alpn != NULL) {
      memcpy(job->alpn, config->alpn, alpn_length + 1u);
      job->config.alpn = job->alpn;
    }
    portENTER_CRITICAL(&s_esp_net_mux);
    job->submitted = 1;
    portEXIT_CRITICAL(&s_esp_net_mux);
    (void)xSemaphoreGive(job->worker->wake);
    int copied = 0;
    for (;;) {
      if (!esp_net_socket_alive(&lease)) {
        rc = H2_PAL_ERR_CLOSED;
        break;
      }
      if (esp_net_timeout_remaining_ms(deadline) == 0u) {
        rc = H2_PAL_ERR_TIMEOUT;
        break;
      }
      portENTER_CRITICAL(&s_esp_net_mux);
      int completed = job->completed, buffer_ready = job->buffer_ready;
      esp_net_tls_job_request_t request = job->request;
      h2_pal_result_t result = job->result;
      portEXIT_CRITICAL(&s_esp_net_mux);
      if (completed) {
        rc = result;
        break;
      }
      if (!copied && buffer_ready &&
          (job->ca != NULL || config->root_ca_pem_len == 0u)) {
        size_t offset = 0u;
        while (offset < config->root_ca_pem_len) {
          if (!esp_net_socket_alive(&lease)) {
            rc = H2_PAL_ERR_CLOSED;
            break;
          }
          if (esp_net_timeout_remaining_ms(deadline) == 0u) {
            rc = H2_PAL_ERR_TIMEOUT;
            break;
          }
          size_t length = config->root_ca_pem_len - offset;
          if (length > H2_ESP_NET_TLS_MAILBOX_BYTES)
            length = H2_ESP_NET_TLS_MAILBOX_BYTES;
          memcpy(job->ca + offset, config->root_ca_pem + offset, length);
          offset += length;
        }
        if (rc != H2_PAL_OK) break;
        if (job->ca != NULL) job->ca[offset] = '\0';
        copied = 1;
        portENTER_CRITICAL(&s_esp_net_mux);
        job->input_ready = 1;
        portEXIT_CRITICAL(&s_esp_net_mux);
        (void)xSemaphoreGive(job->worker->wake);
      } else if (request == ESP_NET_TLS_JOB_SEND ||
                 request == ESP_NET_TLS_JOB_RECV) {
        rc = esp_net_tls_job_service(job, fd, request);
        if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK) break;
      } else
        (void)xSemaphoreTake(
            job->caller_wake,
            esp_net_tls_timeout_ticks(esp_net_timeout_remaining_ms(deadline)));
    }
    if (rc == H2_PAL_OK) {
      rc = esp_net_tls_take_mutex(s_esp_tls_sync->registry_mutex, 1, timeout_ms,
                                  deadline);
      if (rc == H2_PAL_OK) {
        esp_net_tls_socket_t* slot = &s_esp_tls_sockets[job->slot_index];
        if (esp_net_timeout_remaining_ms(deadline) == 0u)
          rc = H2_PAL_ERR_TIMEOUT;
        else if (slot->state != ESP_NET_TLS_SOCKET_CONFIGURING ||
                 s_esp_tls_sync->generations[job->slot_index] !=
                     job->generation)
          rc = H2_PAL_ERR_CLOSED;
        else
          rc = esp_net_socket_attach(&lease, (int)job->slot_index,
                                     job->generation, 0);
        if (rc == H2_PAL_OK) {
          slot->fd = fd;
          mbedtls_ssl_set_bio(&slot->ssl, slot, esp_net_tls_send_raw,
                              esp_net_tls_recv_raw, NULL);
          slot->state = ESP_NET_TLS_SOCKET_ACTIVE;
          *out = fd;
        }
        (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
      }
    }
    esp_net_tls_job_decide(job, rc == H2_PAL_OK ? ESP_NET_TLS_JOB_ACCEPTED
                                                : ESP_NET_TLS_JOB_ABANDONED);
  }
  if (!esp_net_socket_alive(&lease) && rc != H2_PAL_OK) rc = H2_PAL_ERR_CLOSED;
  esp_net_socket_unpin(&lease, 1);
  return rc;
}

#endif
