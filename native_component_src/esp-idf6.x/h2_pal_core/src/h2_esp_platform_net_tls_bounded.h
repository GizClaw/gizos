#ifndef H2_ESP_PLATFORM_NET_TLS_BOUNDED_H
#define H2_ESP_PLATFORM_NET_TLS_BOUNDED_H

/* Included after the Net TLS implementation. Contexts stay in their reserved
 * slots: mbedTLS contains internal pointers and cannot be moved by value.
 * These process-lifetime workers follow the Net resolver's SDK task policy.
 * A stuck SDK call retains one worker, one slot and its owned job; it never
 * retains the caller's fd, config, task handle or stack. */
#define H2_ESP_NET_TLS_WORKER_MAX 2u
#define H2_ESP_NET_TLS_WORKER_STACK_BYTES 8192u
#define H2_ESP_NET_TLS_SERVER_NAME_MAX 253u
#define H2_ESP_NET_TLS_CA_MAX (64u * 1024u)
#define H2_ESP_NET_TLS_MAILBOX_BYTES 2048u

typedef struct esp_net_tls_job esp_net_tls_job_t;
typedef struct esp_net_tls_worker {
  SemaphoreHandle_t wake;
  TaskHandle_t task;
  /* Registry-locked; non-NULL through all cleanup, including SDK free calls. */
  esp_net_tls_job_t* job;
} esp_net_tls_worker_t;

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

struct esp_net_tls_job {
  esp_net_tls_worker_t* worker;
  SemaphoreHandle_t caller_wake;
  size_t slot_index;
  uint32_t generation;
  uint64_t deadline_ms;
  h2_pal_net_tls_config_t config;
  char server_name[H2_ESP_NET_TLS_SERVER_NAME_MAX + 1u];
  char alpn[H2_ESP_NET_TLS_ALPN_LEN];
  /* Control fields are protected by s_esp_tls_job_mux. The request/reply
   * transitions transfer exclusive mailbox ownership without copying under
   * a critical section. Only the worker touches the mbedTLS context. */
  esp_net_tls_job_decision_t decision;
  esp_net_tls_job_request_t request;
  int completed;
  h2_pal_result_t result;
  size_t io_length;
  int io_result;
  uint8_t io[H2_ESP_NET_TLS_MAILBOX_BYTES];
  uint8_t ca[];
};

static portMUX_TYPE s_esp_tls_job_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_net_tls_worker_t s_esp_tls_workers[H2_ESP_NET_TLS_WORKER_MAX];
/* fd + 1, so zero means no TLS identity even before provider initialization.
 * Updates accompany slot reserve/free/publish while holding the registry.
 * This small mirror lets close prove that an unwrapped fd is plain without
 * waiting on a registry lock that just exhausted its TLS wrap deadline. */
static uint32_t s_esp_tls_fd_identities[H2_ESP_NET_TLS_SOCKET_MAX];

static void esp_net_tls_track(size_t index, h2_pal_net_socket_t fd) {
  portENTER_CRITICAL(&s_esp_tls_job_mux);
  s_esp_tls_fd_identities[index] = fd < 0 ? 0u : (uint32_t)fd + 1u;
  portEXIT_CRITICAL(&s_esp_tls_job_mux);
}

static int esp_net_tls_has_identity(h2_pal_net_socket_t fd) {
  int found = 0;
  portENTER_CRITICAL(&s_esp_tls_job_mux);
  for (size_t i = 0u; i < H2_ESP_NET_TLS_SOCKET_MAX; ++i)
    found |= s_esp_tls_fd_identities[i] == (uint32_t)fd + 1u;
  portEXIT_CRITICAL(&s_esp_tls_job_mux);
  return found;
}

static esp_net_tls_job_decision_t esp_net_tls_job_decision(
    esp_net_tls_job_t* job) {
  portENTER_CRITICAL(&s_esp_tls_job_mux);
  esp_net_tls_job_decision_t decision = job->decision;
  portEXIT_CRITICAL(&s_esp_tls_job_mux);
  return decision;
}

/* This is the caller's final job access. Signal a process-lifetime semaphore,
 * so the worker can free the job immediately after observing the decision. */
static void esp_net_tls_job_decide(esp_net_tls_job_t* job,
                                   esp_net_tls_job_decision_t decision) {
  esp_net_tls_worker_t* worker = job->worker;
  portENTER_CRITICAL(&s_esp_tls_job_mux);
  job->decision = decision;
  portEXIT_CRITICAL(&s_esp_tls_job_mux);
  (void)xSemaphoreGive(worker->wake);
}

static int esp_net_tls_job_io(esp_net_tls_job_t* job,
                              esp_net_tls_job_request_t request,
                              const unsigned char* send_data,
                              unsigned char* recv_data, size_t length) {
  if (length > sizeof(job->io)) length = sizeof(job->io);
  if (request == ESP_NET_TLS_JOB_SEND) memcpy(job->io, send_data, length);
  job->io_length = length;
  portENTER_CRITICAL(&s_esp_tls_job_mux);
  job->request = request;
  portEXIT_CRITICAL(&s_esp_tls_job_mux);
  (void)xSemaphoreGive(job->caller_wake);
  for (;;) {
    portENTER_CRITICAL(&s_esp_tls_job_mux);
    int abandoned = job->decision == ESP_NET_TLS_JOB_ABANDONED;
    int replied = job->request == ESP_NET_TLS_JOB_REPLIED;
    portEXIT_CRITICAL(&s_esp_tls_job_mux);
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
    if (result == 0) {
      return job->config.verify == H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY ||
                     mbedtls_ssl_get_verify_result(&slot->ssl) == 0u
                 ? H2_PAL_OK
                 : H2_PAL_ERR_TLS_VERIFY;
    }
    if (result != MBEDTLS_ERR_SSL_WANT_READ &&
        result != MBEDTLS_ERR_SSL_WANT_WRITE) {
      return result == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
                 ? H2_PAL_ERR_TLS_VERIFY
                 : H2_PAL_ERR_IO;
    }
    /* BIO handles socket readiness through the caller. A transient SDK WANT
     * result must not spin or access the reserved slot's fd (which is -1). */
    (void)xSemaphoreTake(job->worker->wake, 1u);
  }
}

static void esp_net_tls_job_worker(void* user) {
  esp_net_tls_worker_t* worker = user;
  for (;;) {
    (void)xSemaphoreTake(worker->wake, portMAX_DELAY);
    /* Submission and reclamation use the registry lock. Taking it here does
     * not affect caller deadlines; no worker ever owns the input socket. */
    (void)xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY);
    esp_net_tls_job_t* job = worker->job;
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
    if (job == NULL) continue;
    size_t index = job->slot_index;
    esp_net_tls_socket_t* slot = &s_esp_tls_sockets[index];
    int initialized = esp_net_tls_job_decision(job) == ESP_NET_TLS_JOB_WAITING;
    h2_pal_result_t rc = H2_PAL_ERR_TIMEOUT;
    if (initialized) {
      rc = esp_net_tls_setup(slot, &job->config);
      if (rc == H2_PAL_OK) rc = esp_net_tls_job_handshake(job, slot);
    }
    if (esp_net_timeout_remaining_ms(job->deadline_ms) == 0u)
      rc = H2_PAL_ERR_TIMEOUT;
    portENTER_CRITICAL(&s_esp_tls_job_mux);
    job->result = rc;
    job->completed = 1;
    portEXIT_CRITICAL(&s_esp_tls_job_mux);
    (void)xSemaphoreGive(job->caller_wake);

    esp_net_tls_job_decision_t decision;
    while ((decision = esp_net_tls_job_decision(job)) ==
           ESP_NET_TLS_JOB_WAITING)
      (void)xSemaphoreTake(worker->wake, portMAX_DELAY);
    /* completed means no more context access until the caller's decision.
     * Accepted contexts belong to the active slot. Abandoned setup contexts
     * are private and can be destroyed outside the registry lock. */
    if (decision == ESP_NET_TLS_JOB_ABANDONED && initialized) {
      mbedtls_ssl_free(&slot->ssl);
      mbedtls_ssl_config_free(&slot->config);
      mbedtls_x509_crt_free(&slot->ca);
    }
    vSemaphoreDelete(job->caller_wake);
    h2_pal_mem_free(h2_esp_platform_default_allocator(), job);
    (void)xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY);
    if (decision == ESP_NET_TLS_JOB_ABANDONED) {
      memset(slot, 0, sizeof(*slot));
      slot->fd = -1;
      s_esp_tls_sync->generations[index] += 1u;
    }
    worker->job = NULL;
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
  }
}

/* Only the caller accesses fd. An outstanding request owns the mailbox until
 * the caller publishes REPLIED or relinquishes the whole job. */
static int esp_net_tls_job_service(esp_net_tls_job_t* job, int fd,
                                   esp_net_tls_job_request_t request) {
  int result = request == ESP_NET_TLS_JOB_SEND
                   ? send(fd, job->io, job->io_length, MSG_DONTWAIT)
                   : recv(fd, job->io, job->io_length, MSG_DONTWAIT);
  if (result < 0 &&
      (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
    uint32_t remaining = esp_net_timeout_remaining_ms(job->deadline_ms);
    if (remaining == 0u) return H2_PAL_ERR_TIMEOUT;
    return h2_esp_net_wait_fd(fd, request == ESP_NET_TLS_JOB_SEND, remaining);
  }
  job->io_result = result >= 0 ? result
                   : request == ESP_NET_TLS_JOB_SEND
                       ? MBEDTLS_ERR_NET_SEND_FAILED
                       : MBEDTLS_ERR_NET_RECV_FAILED;
  portENTER_CRITICAL(&s_esp_tls_job_mux);
  job->request = ESP_NET_TLS_JOB_REPLIED;
  portEXIT_CRITICAL(&s_esp_tls_job_mux);
  (void)xSemaphoreGive(job->worker->wake);
  return H2_PAL_OK;
}

static h2_pal_result_t esp_net_tls_wrap_bounded(
    void* user, h2_pal_net_socket_t fd, const h2_pal_net_tls_config_t* config,
    uint32_t timeout_ms, h2_pal_net_socket_t* out_socket) {
  (void)user;
  uint64_t deadline = esp_net_now_ms() + timeout_ms;
  if (out_socket != NULL) *out_socket = -1;
  if (fd < 0 || config == NULL || out_socket == NULL || timeout_ms == 0u ||
      config->server_name == NULL || config->server_name[0] == '\0' ||
      (config->root_ca_pem == NULL && config->root_ca_pem_len != 0u) ||
      (config->verify != H2_PAL_NET_TLS_VERIFY_DEFAULT &&
       config->verify != H2_PAL_NET_TLS_VERIFY_REQUIRED &&
       config->verify != H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY))
    return H2_PAL_ERR_INVALID_ARG;
  size_t name_length =
      strnlen(config->server_name, H2_ESP_NET_TLS_SERVER_NAME_MAX + 1u);
  size_t alpn_length = config->alpn == NULL
                           ? 0u
                           : strnlen(config->alpn, H2_ESP_NET_TLS_ALPN_LEN);
  if (name_length > H2_ESP_NET_TLS_SERVER_NAME_MAX ||
      alpn_length >= H2_ESP_NET_TLS_ALPN_LEN ||
      config->root_ca_pem_len > H2_ESP_NET_TLS_CA_MAX)
    return H2_PAL_ERR_NO_SPACE;
  /* The API getter initializes process-lifetime TLS synchronization. Do not
   * retry lazy provider initialization inside the bounded operation. */
  int rc = esp_net_tls_take_mutex(
      s_esp_tls_sync == NULL ? NULL : s_esp_tls_sync->registry_mutex, 1,
      timeout_ms, deadline);
  if (rc != H2_PAL_OK) return rc;
  if (esp_net_tls_find(fd) >= 0) {
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
    return H2_PAL_ERR_INVALID_STATE;
  }
  int index = esp_net_tls_find_free();
  esp_net_tls_worker_t* worker = NULL;
  for (size_t i = 0u; i < H2_ESP_NET_TLS_WORKER_MAX; ++i) {
    if (s_esp_tls_workers[i].job == NULL) {
      worker = &s_esp_tls_workers[i];
      break;
    }
  }
  if (index < 0 || worker == NULL) {
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
    return H2_PAL_ERR_NO_SPACE;
  }
  /* Allocation/copy is capped, and capacity remains registry-locked so
   * concurrent callers cannot allocate more than two jobs. */
  esp_net_tls_job_t* job =
      h2_pal_mem_alloc(h2_esp_platform_default_allocator(),
                       sizeof(*job) + config->root_ca_pem_len);
  if (job == NULL) {
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(job, 0, sizeof(*job));
  job->worker = worker;
  job->slot_index = (size_t)index;
  job->deadline_ms = deadline;
  job->config = *config;
  memcpy(job->server_name, config->server_name, name_length + 1u);
  job->config.server_name = job->server_name;
  if (config->alpn != NULL) {
    memcpy(job->alpn, config->alpn, alpn_length + 1u);
    job->config.alpn = job->alpn;
  }
  if (config->root_ca_pem_len != 0u) {
    memcpy(job->ca, config->root_ca_pem, config->root_ca_pem_len);
    job->config.root_ca_pem = job->ca;
  } else {
    job->config.root_ca_pem = NULL;
  }
  job->caller_wake = xSemaphoreCreateBinary();
  if (worker->wake == NULL) worker->wake = xSemaphoreCreateBinary();
  rc = H2_PAL_ERR_NO_MEMORY;
  if (job->caller_wake != NULL && worker->wake != NULL) {
    rc = H2_PAL_OK;
    if (worker->task == NULL) {
      TaskHandle_t task = NULL;
      if (xTaskCreate(esp_net_tls_job_worker, "h2_tls",
                      H2_ESP_NET_TLS_WORKER_STACK_BYTES, worker,
                      tskIDLE_PRIORITY + 1u, &task) != pdPASS)
        rc = H2_PAL_ERR_TASK;
      else
        worker->task = task;
    }
  }
  if (rc == H2_PAL_OK && esp_net_timeout_remaining_ms(deadline) == 0u)
    rc = H2_PAL_ERR_TIMEOUT;
  if (rc != H2_PAL_OK) {
    if (job->caller_wake != NULL) vSemaphoreDelete(job->caller_wake);
    h2_pal_mem_free(h2_esp_platform_default_allocator(), job);
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
    return rc;
  }
  esp_net_tls_socket_t* slot = &s_esp_tls_sockets[index];
  memset(slot, 0, sizeof(*slot));
  slot->state = ESP_NET_TLS_SOCKET_CONFIGURING;
  slot->fd = -1;
  job->generation = ++s_esp_tls_sync->generations[index];
  worker->job = job;
  (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
  (void)xSemaphoreGive(worker->wake);

  for (;;) {
    if (esp_net_timeout_remaining_ms(deadline) == 0u) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    portENTER_CRITICAL(&s_esp_tls_job_mux);
    int completed = job->completed;
    esp_net_tls_job_request_t request = job->request;
    h2_pal_result_t result = job->result;
    portEXIT_CRITICAL(&s_esp_tls_job_mux);
    if (completed) {
      rc = result;
      break;
    }
    if (request == ESP_NET_TLS_JOB_SEND || request == ESP_NET_TLS_JOB_RECV) {
      rc = esp_net_tls_job_service(job, fd, request);
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK) break;
    } else {
      (void)xSemaphoreTake(
          job->caller_wake,
          esp_net_tls_timeout_ticks(esp_net_timeout_remaining_ms(deadline)));
    }
  }
  if (rc == H2_PAL_OK) {
    rc = esp_net_tls_take_mutex(s_esp_tls_sync->registry_mutex, 1, timeout_ms,
                                deadline);
    if (rc == H2_PAL_OK) {
      if (esp_net_timeout_remaining_ms(deadline) == 0u) {
        rc = H2_PAL_ERR_TIMEOUT;
      } else if (slot->state != ESP_NET_TLS_SOCKET_CONFIGURING ||
                 s_esp_tls_sync->generations[index] != job->generation) {
        rc = H2_PAL_ERR_CLOSED;
      } else {
        /* completed is a handshake/context ownership barrier, not merely a
         * setup notification. No worker can touch this context after it. */
        slot->fd = fd;
        mbedtls_ssl_set_bio(&slot->ssl, slot, esp_net_tls_send_raw,
                            esp_net_tls_recv_raw, NULL);
        esp_net_tls_track((size_t)index, fd);
        slot->state = ESP_NET_TLS_SOCKET_ACTIVE;
        *out_socket = fd;
      }
      (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
    }
  }
  esp_net_tls_job_decide(job, rc == H2_PAL_OK ? ESP_NET_TLS_JOB_ACCEPTED
                                              : ESP_NET_TLS_JOB_ABANDONED);
  return rc;
}

#endif
