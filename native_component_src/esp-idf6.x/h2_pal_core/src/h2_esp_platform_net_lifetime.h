#ifndef H2_ESP_PLATFORM_NET_LIFETIME_H
#define H2_ESP_PLATFORM_NET_LIFETIME_H

/* Public handles remain native socket numbers. A lease pins that native fd
 * until the entire public operation exits, so logical close cannot redirect
 * an in-flight operation to a newly opened socket with the same number. */
enum {
  ESP_NET_SOCKET_IO = 0,
  ESP_NET_SOCKET_LEGACY_WRAP = 1,
  ESP_NET_SOCKET_BOUNDED_WRAP = 2,
};
enum {
  ESP_NET_SOCKET_FRESH = 0,
  ESP_NET_SOCKET_LISTENER,
  /* Includes a pending/failed connect attempt: native close may still wait. */
  ESP_NET_SOCKET_CONNECTION,
};
typedef struct esp_net_socket_record {
  int used, closed, wrapping, shutdown_started, reaping, unpublished;
  int fd, type, role, tls_slot, abortive_close, close_prepared;
  uint32_t generation, tls_generation;
  size_t references;
} esp_net_socket_record_t;
typedef struct esp_net_socket_lease {
  size_t index;
  uint32_t generation;
  int fd;
} esp_net_socket_lease_t;

static portMUX_TYPE s_esp_net_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_net_socket_record_t s_esp_net_records[CONFIG_LWIP_MAX_SOCKETS];
static SemaphoreHandle_t s_esp_net_reaper_wake;
static TaskHandle_t s_esp_net_reaper_task;
static SemaphoreHandle_t s_esp_tls_cleanup_wake;
static TaskHandle_t s_esp_tls_cleanup_task;
typedef struct esp_net_tls_cleanup {
  int pending;
  uint32_t generation;
} esp_net_tls_cleanup_t;
static esp_net_tls_cleanup_t s_esp_tls_cleanup[H2_ESP_NET_TLS_SOCKET_MAX];
static int s_esp_tls_cleanup_busy;
static size_t s_esp_tls_cleanup_cursor, s_esp_net_reap_cursor;
static int esp_net_socket_reap_one(void);
#define H2_ESP_NET_CANCEL_POLL_MS 20u
static void esp_net_tls_jobs_wake(void);
static h2_pal_result_t esp_net_tls_wrap(void*, h2_pal_net_socket_t,
                                        const h2_pal_net_tls_config_t*,
                                        uint32_t, h2_pal_net_socket_t*);

static int esp_net_socket_has_executor(void) {
  portENTER_CRITICAL(&s_esp_net_mux);
  int ready = s_esp_net_reaper_task != NULL || s_esp_tls_cleanup_task != NULL;
  portEXIT_CRITICAL(&s_esp_net_mux);
  return ready;
}
static int esp_net_socket_has_reaper(void) {
  portENTER_CRITICAL(&s_esp_net_mux);
  int ready = s_esp_net_reaper_task != NULL;
  portEXIT_CRITICAL(&s_esp_net_mux);
  return ready;
}
static void esp_net_socket_kick(void) {
  portENTER_CRITICAL(&s_esp_net_mux);
  SemaphoreHandle_t wake = s_esp_net_reaper_task != NULL
                               ? s_esp_net_reaper_wake
                               : s_esp_tls_cleanup_wake;
  portEXIT_CRITICAL(&s_esp_net_mux);
  if (wake != NULL) (void)xSemaphoreGive(wake);
}
/* Caller holds s_esp_net_mux. Only these native closes have no protocol wait.
 */
static int esp_net_socket_inline_close(const esp_net_socket_record_t* record) {
  return record->tls_slot < 0 &&
         (record->type != SOCK_STREAM ||
          record->role != ESP_NET_SOCKET_CONNECTION || record->abortive_close);
}

static int esp_net_socket_provision(int fd, int type, int role) {
  if (type == SOCK_STREAM && role != ESP_NET_SOCKET_LISTENER) {
    const struct linger abortive = {.l_onoff = 1, .l_linger = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive)) != 0)
      return H2_PAL_ERR_UNAVAILABLE;
  }
  return H2_PAL_OK;
}
/* Caller holds s_esp_net_mux. A reserved accept record has no fd yet; its
 * caller reference and unpublished flag exclude background/native users. */
static int esp_net_socket_create_locked(int fd, int type, int role,
                                        size_t references, int unpublished,
                                        esp_net_socket_lease_t* owner) {
  for (size_t i = 0u; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
    esp_net_socket_record_t* record = &s_esp_net_records[i];
    if (!record->used) {
      uint32_t generation = record->generation + 1u;
      *record = (esp_net_socket_record_t){.used = 1,
                                          .fd = fd,
                                          .type = type,
                                          .role = role,
                                          .tls_slot = -1,
                                          .generation = generation,
                                          .references = references,
                                          .unpublished = unpublished};
      if (owner != NULL) *owner = (esp_net_socket_lease_t){i, generation, fd};
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NO_SPACE;
}
static int esp_net_socket_register(int fd, int type, int role) {
  if (type == SOCK_STREAM && role == ESP_NET_SOCKET_CONNECTION &&
      !esp_net_socket_has_executor())
    return H2_PAL_ERR_UNAVAILABLE;
  int result = esp_net_socket_provision(fd, type, role);
  if (result != H2_PAL_OK) return result;
  portENTER_CRITICAL(&s_esp_net_mux);
  result = esp_net_socket_create_locked(fd, type, role, 0u, 0, NULL);
  portEXIT_CRITICAL(&s_esp_net_mux);
  return result;
}
static int esp_net_socket_reserve_accept(esp_net_socket_lease_t* owner) {
  portENTER_CRITICAL(&s_esp_net_mux);
  int result = esp_net_socket_create_locked(
      -1, SOCK_STREAM, ESP_NET_SOCKET_CONNECTION, 1u, 1, owner);
  portEXIT_CRITICAL(&s_esp_net_mux);
  return result;
}
static void esp_net_socket_bind_accept(void* context, int fd) {
  esp_net_socket_lease_t* owner = context;
  portENTER_CRITICAL(&s_esp_net_mux);
  s_esp_net_records[owner->index].fd = fd;
  portEXIT_CRITICAL(&s_esp_net_mux);
  owner->fd = fd;
}
static int esp_net_socket_finish_accept(const esp_net_socket_lease_t* listener,
                                        const esp_net_socket_lease_t* owner,
                                        int result) {
  portENTER_CRITICAL(&s_esp_net_mux);
  esp_net_socket_record_t* parent = &s_esp_net_records[listener->index];
  esp_net_socket_record_t* record = &s_esp_net_records[owner->index];
  if (!parent->used || parent->closed ||
      parent->generation != listener->generation || record->closed)
    result = H2_PAL_ERR_CLOSED;
  record->unpublished = 0;
  record->references = 0u;
  if (result != H2_PAL_OK) record->closed = 1;
  portEXIT_CRITICAL(&s_esp_net_mux);
  if (result != H2_PAL_OK) esp_net_socket_kick();
  return result;
}
static void esp_net_socket_cancel_accept(const esp_net_socket_lease_t* owner) {
  /* No native fd was created. Releasing this reservation is CPU-only. */
  portENTER_CRITICAL(&s_esp_net_mux);
  s_esp_net_records[owner->index].used = 0;
  portEXIT_CRITICAL(&s_esp_net_mux);
}

static int esp_net_socket_pin(int fd, int type, int wrapping,
                              esp_net_socket_lease_t* lease) {
  int result = H2_PAL_ERR_CLOSED;
  if (fd < 0) return H2_PAL_ERR_INVALID_ARG;
  portENTER_CRITICAL(&s_esp_net_mux);
  for (size_t i = 0u; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
    esp_net_socket_record_t* record = &s_esp_net_records[i];
    if (!record->used || record->fd != fd) continue;
    if (!record->closed) {
      if (record->unpublished || record->type != type || record->wrapping ||
          (wrapping && (record->role != ESP_NET_SOCKET_CONNECTION ||
                        record->tls_slot >= 0 || record->references != 0u))) {
        result = H2_PAL_ERR_INVALID_STATE;
      } else {
        ++record->references;
        record->wrapping = wrapping;
        record->abortive_close = wrapping == ESP_NET_SOCKET_BOUNDED_WRAP;
        *lease = (esp_net_socket_lease_t){i, record->generation, fd};
        result = H2_PAL_OK;
      }
    }
    break;
  }
  portEXIT_CRITICAL(&s_esp_net_mux);
  return result;
}

static int esp_net_socket_alive(const esp_net_socket_lease_t* lease) {
  portENTER_CRITICAL(&s_esp_net_mux);
  esp_net_socket_record_t* record = &s_esp_net_records[lease->index];
  int alive = record->used && !record->closed &&
              record->generation == lease->generation;
  portEXIT_CRITICAL(&s_esp_net_mux);
  return alive;
}

static int esp_net_socket_begin_connect(const esp_net_socket_lease_t* lease) {
  int result = H2_PAL_ERR_CLOSED;
  portENTER_CRITICAL(&s_esp_net_mux);
  esp_net_socket_record_t* record = &s_esp_net_records[lease->index];
  if (record->used && !record->closed &&
      record->generation == lease->generation) {
    if (s_esp_net_reaper_task == NULL && s_esp_tls_cleanup_task == NULL)
      result = H2_PAL_ERR_UNAVAILABLE;
    else if (record->role == ESP_NET_SOCKET_LISTENER)
      result = H2_PAL_ERR_INVALID_STATE;
    else {
      record->role = ESP_NET_SOCKET_CONNECTION;
      result = H2_PAL_OK;
    }
  }
  portEXIT_CRITICAL(&s_esp_net_mux);
  return result;
}

/* Only an unsuccessful, unpublished bounded TLS request selects abortive
 * close. Subsequent ordinary use or successful TLS publication restores the
 * logical normal-close profile without putting SDK calls in the timed entry. */
static void esp_net_socket_request_profile(int fd, int abortive) {
  portENTER_CRITICAL(&s_esp_net_mux);
  for (size_t i = 0u; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
    esp_net_socket_record_t* record = &s_esp_net_records[i];
    if (record->used && !record->closed && record->fd == fd &&
        record->type == SOCK_STREAM && record->references == 0u &&
        !record->wrapping && record->tls_slot < 0) {
      record->abortive_close = abortive;
      break;
    }
  }
  portEXIT_CRITICAL(&s_esp_net_mux);
}

static int esp_net_socket_prepare_close(const esp_net_socket_lease_t* lease,
                                        int type, int abortive) {
  portENTER_CRITICAL(&s_esp_net_mux);
  esp_net_socket_record_t* record = &s_esp_net_records[lease->index];
  int owned = record->used && record->generation == lease->generation;
  int prepared = record->close_prepared;
  int role = record->role;
  portEXIT_CRITICAL(&s_esp_net_mux);
  if (!owned) return H2_PAL_ERR_CLOSED;
  if (prepared) return H2_PAL_OK;
  if (type == SOCK_STREAM && role == ESP_NET_SOCKET_CONNECTION && !abortive) {
    const struct linger normal = {.l_onoff = 0, .l_linger = 0};
    if (setsockopt(lease->fd, SOL_SOCKET, SO_LINGER, &normal, sizeof(normal)) !=
        0)
      return H2_PAL_ERR_IO;
  }
  /* Profile restoration must precede shutdown, and is performed once: some
   * native stacks reject another setsockopt after shutdown has completed. */
  portENTER_CRITICAL(&s_esp_net_mux);
  record->close_prepared = 1;
  portEXIT_CRITICAL(&s_esp_net_mux);
  return H2_PAL_OK;
}

static void esp_net_socket_plain_close(const esp_net_socket_lease_t* lease,
                                       int type, int abortive) {
  int ready = esp_net_socket_prepare_close(lease, type, abortive) == H2_PAL_OK;
  portENTER_CRITICAL(&s_esp_net_mux);
  esp_net_socket_record_t* record = &s_esp_net_records[lease->index];
  if (ready)
    record->used = 0;
  else
    record->reaping = 0; /* Keep ownership for the background retry. */
  portEXIT_CRITICAL(&s_esp_net_mux);
  if (ready)
    (void)close(lease->fd);
  else
    esp_net_socket_kick();
}

/* Also used when optional TLS/reaper initialization failed. Plain network
 * operations remain available. No SDK operation runs inside the critical
 * section, and a native fd cannot be freed while any operation still pins it.
 */
static void esp_net_socket_unpin(const esp_net_socket_lease_t* lease,
                                 int wrapping) {
  int close_fd = -1, type = 0, abortive = 0;
  portENTER_CRITICAL(&s_esp_net_mux);
  esp_net_socket_record_t* record = &s_esp_net_records[lease->index];
  if (record->used && record->generation == lease->generation) {
    if (wrapping) record->wrapping = 0;
    --record->references;
    if (record->closed && record->references == 0u && !record->reaping &&
        wrapping != ESP_NET_SOCKET_BOUNDED_WRAP &&
        esp_net_socket_inline_close(record)) {
      record->reaping = 1;
      close_fd = record->fd;
      type = record->type;
      abortive = record->abortive_close;
    }
  }
  portEXIT_CRITICAL(&s_esp_net_mux);
  if (close_fd >= 0) esp_net_socket_plain_close(lease, type, abortive);
  esp_net_socket_kick();
}

/* Registry is held. Attach even a logically closed legacy result, so the
 * reaper owns that context after its synchronous caller finally exits. */
static int esp_net_socket_attach(const esp_net_socket_lease_t* lease, int slot,
                                 uint32_t generation, int allow_closed) {
  int result = H2_PAL_ERR_CLOSED;
  portENTER_CRITICAL(&s_esp_net_mux);
  esp_net_socket_record_t* record = &s_esp_net_records[lease->index];
  if (record->used && record->generation == lease->generation &&
      (!record->closed || allow_closed)) {
    record->tls_slot = slot;
    record->tls_generation = generation;
    record->abortive_close = 0;
    if (!record->closed) result = H2_PAL_OK;
  }
  portEXIT_CRITICAL(&s_esp_net_mux);
  return result;
}

/* Queue admission/claim is bounded by the eight CLOSING slots. Caller holds
 * s_esp_net_mux. A fair cursor keeps newly reused low slots from starving old
 * work. */
static int esp_net_tls_cleanup_take_locked(size_t* index,
                                           uint32_t* generation) {
  for (size_t offset = 0u; offset < H2_ESP_NET_TLS_SOCKET_MAX; ++offset) {
    size_t i = (s_esp_tls_cleanup_cursor + offset) % H2_ESP_NET_TLS_SOCKET_MAX;
    if (s_esp_tls_cleanup[i].pending) {
      *index = i;
      *generation = s_esp_tls_cleanup[i].generation;
      s_esp_tls_cleanup[i].pending = 0;
      s_esp_tls_cleanup_cursor = (i + 1u) % H2_ESP_NET_TLS_SOCKET_MAX;
      return 1;
    }
  }
  return 0;
}
static void esp_net_tls_cleanup_slot(size_t i, uint32_t generation) {
  (void)xSemaphoreTake(s_esp_tls_sync->io_mutexes[i], portMAX_DELAY);
  (void)xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY);
  esp_net_tls_socket_t* slot = &s_esp_tls_sockets[i];
  int owned = slot->state == ESP_NET_TLS_SOCKET_CLOSING && slot->fd == -1 &&
              s_esp_tls_sync->generations[i] == generation;
  (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
  if (owned) {
    mbedtls_ssl_free(&slot->ssl);
    mbedtls_ssl_config_free(&slot->config);
    mbedtls_x509_crt_free(&slot->ca);
    (void)xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY);
    memset(slot, 0, sizeof(*slot));
    slot->fd = -1;
    ++s_esp_tls_sync->generations[i];
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
  }
  esp_net_tls_release(i);
}
static void esp_net_tls_cleanup_worker(void* unused) {
  (void)unused;
  for (;;) {
    /* During partial initialization this executor also owns pending native
     * closes. No TCP connection can be published without at least one executor.
     */
    if (!esp_net_socket_has_reaper()) {
      portENTER_CRITICAL(&s_esp_net_mux);
      s_esp_tls_cleanup_busy = 1;
      portEXIT_CRITICAL(&s_esp_net_mux);
      esp_net_tls_jobs_wake();
      int progressed = esp_net_socket_reap_one();
      portENTER_CRITICAL(&s_esp_net_mux);
      s_esp_tls_cleanup_busy = 0;
      portEXIT_CRITICAL(&s_esp_net_mux);
      if (progressed) continue;
    }
    size_t index = 0u;
    uint32_t generation = 0u;
    portENTER_CRITICAL(&s_esp_net_mux);
    int claimed = esp_net_tls_cleanup_take_locked(&index, &generation);
    if (claimed) s_esp_tls_cleanup_busy = 1;
    portEXIT_CRITICAL(&s_esp_net_mux);
    if (claimed) {
      /* Wake on the idle->busy transition too. Earlier admission wakes may
       * already have been consumed by helpers while stealing was disabled. */
      esp_net_tls_jobs_wake();
      esp_net_tls_cleanup_slot(index, generation);
      portENTER_CRITICAL(&s_esp_net_mux);
      s_esp_tls_cleanup_busy = 0;
      portEXIT_CRITICAL(&s_esp_net_mux);
      continue;
    }
    (void)xSemaphoreTake(s_esp_tls_cleanup_wake, portMAX_DELAY);
  }
}

static void esp_net_close(void* user, h2_pal_net_socket_t fd) {
  (void)user;
  if (fd < 0) return;
  int close_fd = -1, type = 0, abortive = 0;
  esp_net_socket_lease_t close_lease = {0};
  portENTER_CRITICAL(&s_esp_net_mux);
  for (size_t i = 0u; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
    esp_net_socket_record_t* record = &s_esp_net_records[i];
    if (record->used && record->fd == fd) {
      record->closed = 1;
      if (!record->reaping && record->references == 0u &&
          esp_net_socket_inline_close(record)) {
        record->reaping = 1;
        close_fd = fd;
        type = record->type;
        abortive = record->abortive_close;
        close_lease = (esp_net_socket_lease_t){i, record->generation, fd};
      }
      break;
    }
  }
  portEXIT_CRITICAL(&s_esp_net_mux);
  esp_net_tls_jobs_wake();
  esp_net_socket_kick();
  if (close_fd >= 0) esp_net_socket_plain_close(&close_lease, type, abortive);
}

/* A single claimed native operation, executable by the normal reaper or the
 * cleanup executor during partial initialization. All SDK work is outside
 * s_esp_net_mux, with a reference or an exclusive reclamation claim. */
static int esp_net_socket_reap_one(void) {
  portENTER_CRITICAL(&s_esp_net_mux);
  size_t start = s_esp_net_reap_cursor;
  portEXIT_CRITICAL(&s_esp_net_mux);
  for (size_t offset = 0u; offset < CONFIG_LWIP_MAX_SOCKETS; ++offset) {
    size_t i = (start + offset) % CONFIG_LWIP_MAX_SOCKETS;
    int fd = -1, shutdown_fd = -1, slot_index = -1, type = 0, abortive = 0;
    uint32_t generation = 0u, tls_generation = 0u;
    portENTER_CRITICAL(&s_esp_net_mux);
    esp_net_socket_record_t* record = &s_esp_net_records[i];
    if (record->used && record->closed && !record->reaping &&
        !record->unpublished) {
      if (record->references == 0u) {
        record->reaping = 1;
        fd = record->fd;
        slot_index = record->tls_slot;
        tls_generation = record->tls_generation;
      } else if (!record->shutdown_started) {
        record->shutdown_started = 1;
        ++record->references;
        shutdown_fd = record->fd;
      }
      generation = record->generation;
      type = record->type;
      abortive = record->abortive_close;
      if (fd >= 0 || shutdown_fd >= 0)
        s_esp_net_reap_cursor = (i + 1u) % CONFIG_LWIP_MAX_SOCKETS;
    }
    portEXIT_CRITICAL(&s_esp_net_mux);
    if (shutdown_fd >= 0) {
      esp_net_socket_lease_t lease = {i, generation, shutdown_fd};
      int prepared =
          esp_net_socket_prepare_close(&lease, type, abortive) == H2_PAL_OK;
      if (prepared)
        (void)shutdown(shutdown_fd, SHUT_RDWR);
      else {
        portENTER_CRITICAL(&s_esp_net_mux);
        record->shutdown_started = 0;
        portEXIT_CRITICAL(&s_esp_net_mux);
      }
      esp_net_socket_unpin(&lease, 0);
      if (!prepared) {
        vTaskDelay(1u);
        esp_net_socket_kick();
      }
      return 1;
    }
    if (fd < 0) continue;
    esp_net_socket_lease_t lease = {i, generation, fd};
    if (esp_net_socket_prepare_close(&lease, type, abortive) != H2_PAL_OK) {
      portENTER_CRITICAL(&s_esp_net_mux);
      record->reaping = 0;
      portEXIT_CRITICAL(&s_esp_net_mux);
      vTaskDelay(1u);
      esp_net_socket_kick();
      return 1;
    }
    esp_net_tls_socket_t* slot = NULL;
    if (slot_index >= 0) {
      (void)xSemaphoreTake(s_esp_tls_sync->io_mutexes[slot_index],
                           portMAX_DELAY);
      (void)xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY);
      esp_net_tls_socket_t* candidate = &s_esp_tls_sockets[slot_index];
      if (candidate->state == ESP_NET_TLS_SOCKET_ACTIVE &&
          candidate->fd == fd &&
          s_esp_tls_sync->generations[slot_index] == tls_generation) {
        candidate->state = ESP_NET_TLS_SOCKET_CLOSING;
        candidate->fd = -1;
        slot = candidate;
      }
      (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
    }
    portENTER_CRITICAL(&s_esp_net_mux);
    record->used = 0;
    portEXIT_CRITICAL(&s_esp_net_mux);
    (void)close(fd);
    if (slot_index >= 0) esp_net_tls_release((size_t)slot_index);
    if (slot != NULL) {
      portENTER_CRITICAL(&s_esp_net_mux);
      s_esp_tls_cleanup[slot_index] =
          (esp_net_tls_cleanup_t){1, tls_generation};
      portEXIT_CRITICAL(&s_esp_net_mux);
      (void)xSemaphoreGive(s_esp_tls_cleanup_wake);
      esp_net_tls_jobs_wake();
    }
    return 1;
  }
  return 0;
}
static void esp_net_socket_reaper(void* unused) {
  (void)unused;
  for (;;) {
    (void)xSemaphoreTake(s_esp_net_reaper_wake, portMAX_DELAY);
    while (esp_net_socket_reap_one()) {
    }
  }
}

/* These constructors publish only fresh/listener/datagram sockets, whose
 * failure close has no connected FIN wait. Accept uses its reserved owner. */
static int esp_net_socket_adopt(int result, int type, int role,
                                h2_pal_net_socket_t* out) {
  if (result != H2_PAL_OK) return result;
  result = esp_net_socket_register(*out, type, role);
  if (result != H2_PAL_OK) {
    (void)close(*out);
    *out = -1;
  }
  return result;
}

static int esp_net_tcp_open_managed(void* user, h2_pal_net_family_t family,
                                    h2_pal_net_socket_t* out) {
  return esp_net_socket_adopt(esp_net_tcp_open(user, family, out), SOCK_STREAM,
                              ESP_NET_SOCKET_FRESH, out);
}
static int esp_net_tcp_open_bound_managed(void* user,
                                          h2_pal_net_family_t family,
                                          const h2_pal_net_bind_t* bind,
                                          h2_pal_net_socket_t* out) {
  return esp_net_socket_adopt(esp_net_tcp_open_bound(user, family, bind, out),
                              SOCK_STREAM, ESP_NET_SOCKET_FRESH, out);
}
static int esp_net_udp_open_managed(void* user, h2_pal_net_family_t family,
                                    uint16_t port, h2_pal_net_socket_t* out,
                                    h2_pal_net_addr_t* address) {
  return esp_net_socket_adopt(
      esp_net_udp_open(user, family, port, out, address), SOCK_DGRAM,
      ESP_NET_SOCKET_FRESH, out);
}
static int esp_net_udp_open_bound_managed(void* user,
                                          h2_pal_net_family_t family,
                                          uint16_t port,
                                          const h2_pal_net_bind_t* bind,
                                          h2_pal_net_socket_t* out,
                                          h2_pal_net_addr_t* address) {
  return esp_net_socket_adopt(
      esp_net_udp_open_bound(user, family, port, bind, out, address),
      SOCK_DGRAM, ESP_NET_SOCKET_FRESH, out);
}
static int esp_net_tcp_listen_managed(void* user, h2_pal_net_family_t family,
                                      uint16_t port,
                                      const h2_pal_net_bind_t* bind,
                                      h2_pal_net_socket_t* out,
                                      h2_pal_net_addr_t* address) {
  return esp_net_socket_adopt(
      esp_net_tcp_listen(user, family, port, bind, out, address), SOCK_STREAM,
      ESP_NET_SOCKET_LISTENER, out);
}

#define H2_ESP_NET_LEASE_BEGIN(kind)                \
  esp_net_socket_lease_t lease;                     \
  int rc = esp_net_socket_pin(fd, kind, 0, &lease); \
  if (rc != H2_PAL_OK) return rc
#define H2_ESP_NET_LEASE_END(expression)                               \
  rc = (expression);                                                   \
  if (rc < 0 && !esp_net_socket_alive(&lease)) rc = H2_PAL_ERR_CLOSED; \
  esp_net_socket_unpin(&lease, 0);                                     \
  return rc

static h2_pal_result_t esp_net_tcp_connect_managed(
    void* user, h2_pal_net_socket_t fd, const h2_pal_net_addr_t* address,
    uint32_t timeout_ms) {
  H2_ESP_NET_LEASE_BEGIN(SOCK_STREAM);
  rc = esp_net_socket_begin_connect(&lease);
  if (rc != H2_PAL_OK) {
    esp_net_socket_unpin(&lease, 0);
    return rc;
  }
  H2_ESP_NET_LEASE_END(esp_net_tcp_connect(user, fd, address, timeout_ms));
}
static h2_pal_result_t esp_net_tcp_accept_managed(void* user,
                                                  h2_pal_net_socket_t fd,
                                                  h2_pal_net_socket_t* out,
                                                  h2_pal_net_addr_t* peer,
                                                  uint32_t timeout_ms) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = -1;
  if (!esp_net_socket_has_executor()) return H2_PAL_ERR_UNAVAILABLE;
  H2_ESP_NET_LEASE_BEGIN(SOCK_STREAM);
  esp_net_socket_lease_t owner;
  rc = esp_net_socket_reserve_accept(&owner);
  if (rc != H2_PAL_OK) {
    esp_net_socket_unpin(&lease, 0);
    return rc;
  }
  uint64_t deadline = esp_net_now_ms() + timeout_ms;
  h2_pal_net_socket_t accepted = -1;
  for (;;) {
    if (!esp_net_socket_alive(&lease)) {
      rc = H2_PAL_ERR_CLOSED;
      break;
    }
    uint32_t remaining =
        timeout_ms == 0u ? 0u : esp_net_timeout_remaining_ms(deadline);
    if (timeout_ms != 0u && remaining == 0u) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    uint32_t slice = remaining > H2_ESP_NET_CANCEL_POLL_MS
                         ? H2_ESP_NET_CANCEL_POLL_MS
                         : remaining;
    rc = esp_net_tcp_accept(&owner, fd, &accepted, peer, slice);
    if (rc != H2_PAL_ERR_TIMEOUT || timeout_ms == 0u) break;
  }
  if (owner.fd >= 0) {
    /* This pre-reserved record owns the unpublished fd before configuration.
     * Cancellation and adoption failure cannot force graceful close into this
     * timed caller or require a fallible second allocation of owner capacity.
     */
    if (rc == H2_PAL_OK)
      rc = esp_net_socket_alive(&lease)
               ? esp_net_socket_provision(accepted, SOCK_STREAM,
                                          ESP_NET_SOCKET_CONNECTION)
               : H2_PAL_ERR_CLOSED;
    rc = esp_net_socket_finish_accept(&lease, &owner, rc);
    if (rc == H2_PAL_OK) *out = accepted;
  } else
    esp_net_socket_cancel_accept(&owner);
  esp_net_socket_unpin(&lease, 0);
  return rc;
}

static int esp_net_tcp_send_managed(void* user, h2_pal_net_socket_t fd,
                                    const uint8_t* data, size_t length) {
  H2_ESP_NET_LEASE_BEGIN(SOCK_STREAM);
  H2_ESP_NET_LEASE_END(esp_net_tcp_send(user, fd, data, length));
}
static int esp_net_tcp_send_timeout_managed(void* user, h2_pal_net_socket_t fd,
                                            const uint8_t* data, size_t length,
                                            uint32_t timeout_ms) {
  H2_ESP_NET_LEASE_BEGIN(SOCK_STREAM);
  H2_ESP_NET_LEASE_END(
      esp_net_tcp_send_timeout(user, fd, data, length, timeout_ms));
}
static int esp_net_tcp_recv_managed(void* user, h2_pal_net_socket_t fd,
                                    uint8_t* data, size_t length,
                                    uint32_t timeout_ms) {
  H2_ESP_NET_LEASE_BEGIN(SOCK_STREAM);
  H2_ESP_NET_LEASE_END(esp_net_tcp_recv(user, fd, data, length, timeout_ms));
}
static int esp_net_udp_sendto_managed(void* user, h2_pal_net_socket_t fd,
                                      const h2_pal_net_addr_t* address,
                                      const uint8_t* data, size_t length) {
  H2_ESP_NET_LEASE_BEGIN(SOCK_DGRAM);
  H2_ESP_NET_LEASE_END(esp_net_udp_sendto(user, fd, address, data, length));
}
static int esp_net_udp_recvfrom_managed(void* user, h2_pal_net_socket_t fd,
                                        h2_pal_net_addr_t* address,
                                        uint8_t* data, size_t length,
                                        uint32_t timeout_ms) {
  H2_ESP_NET_LEASE_BEGIN(SOCK_DGRAM);
  uint64_t deadline = esp_net_now_ms() + timeout_ms;
  for (;;) {
    if (!esp_net_socket_alive(&lease)) {
      rc = H2_PAL_ERR_CLOSED;
      break;
    }
    uint32_t remaining =
        timeout_ms == 0u ? 0u : esp_net_timeout_remaining_ms(deadline);
    if (timeout_ms != 0u && remaining == 0u) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    uint32_t slice = remaining > H2_ESP_NET_CANCEL_POLL_MS
                         ? H2_ESP_NET_CANCEL_POLL_MS
                         : remaining;
    rc = esp_net_udp_recvfrom(user, fd, address, data, length, slice);
    if (rc != H2_PAL_ERR_TIMEOUT || timeout_ms == 0u) break;
  }
  if (rc < 0 && !esp_net_socket_alive(&lease)) rc = H2_PAL_ERR_CLOSED;
  esp_net_socket_unpin(&lease, 0);
  return rc;
}
static int esp_net_udp_join_multicast_managed(
    void* user, h2_pal_net_socket_t fd, const h2_pal_net_addr_t* address) {
  H2_ESP_NET_LEASE_BEGIN(SOCK_DGRAM);
  H2_ESP_NET_LEASE_END(esp_net_udp_join_multicast(user, fd, address));
}
#undef H2_ESP_NET_LEASE_BEGIN
#undef H2_ESP_NET_LEASE_END

static h2_pal_result_t esp_net_tls_wrap_managed(
    void* user, h2_pal_net_socket_t fd, const h2_pal_net_tls_config_t* config,
    uint32_t timeout_ms, h2_pal_net_socket_t* out) {
  if (out != NULL) *out = -1;
  esp_net_socket_request_profile(fd, 0);
  if (!esp_net_tls_available()) {
    /* Compatibility entry may retry opaque initialization. The bounded entry
     * never calls this recovery path. */
    esp_net_tls_init();
    if (!esp_net_tls_available()) return H2_PAL_ERR_UNAVAILABLE;
  }
  esp_net_socket_lease_t lease;
  int rc =
      esp_net_socket_pin(fd, SOCK_STREAM, ESP_NET_SOCKET_LEGACY_WRAP, &lease);
  if (rc != H2_PAL_OK) return rc;
  rc = esp_net_tls_wrap(user, fd, config, timeout_ms, out);
  if (rc == H2_PAL_OK) {
    (void)xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY);
    int index = esp_net_tls_find(fd);
    rc = index < 0 ? H2_PAL_ERR_CLOSED
                   : esp_net_socket_attach(
                         &lease, index, s_esp_tls_sync->generations[index], 1);
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
  } else if (!esp_net_socket_alive(&lease))
    rc = H2_PAL_ERR_CLOSED;
  if (rc != H2_PAL_OK && out != NULL) *out = -1;
  esp_net_socket_unpin(&lease, 1);
  return rc;
}

#endif
