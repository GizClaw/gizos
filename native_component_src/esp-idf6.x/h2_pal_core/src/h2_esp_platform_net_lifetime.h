#ifndef H2_ESP_PLATFORM_NET_LIFETIME_H
#define H2_ESP_PLATFORM_NET_LIFETIME_H

/* Public handles remain native socket numbers. A lease pins that native fd
 * until the entire public operation exits, so logical close cannot redirect
 * an in-flight operation to a newly opened socket with the same number. */
typedef struct esp_net_socket_record {
  int used, closed, wrapping, shutdown_started;
  int fd, type, tls_slot;
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
static void esp_net_tls_jobs_wake(void);
static h2_pal_result_t esp_net_tls_wrap(void*, h2_pal_net_socket_t,
                                        const h2_pal_net_tls_config_t*,
                                        uint32_t, h2_pal_net_socket_t*);

static void esp_net_socket_kick(void) {
  portENTER_CRITICAL(&s_esp_net_mux);
  SemaphoreHandle_t wake = s_esp_net_reaper_wake;
  portEXIT_CRITICAL(&s_esp_net_mux);
  if (wake != NULL) (void)xSemaphoreGive(wake);
}

static int esp_net_socket_register(int fd, int type) {
  int result = H2_PAL_ERR_NO_SPACE;
  portENTER_CRITICAL(&s_esp_net_mux);
  for (size_t i = 0u; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
    esp_net_socket_record_t* record = &s_esp_net_records[i];
    if (!record->used) {
      uint32_t generation = record->generation + 1u;
      *record = (esp_net_socket_record_t){.used = 1,
                                          .fd = fd,
                                          .type = type,
                                          .tls_slot = -1,
                                          .generation = generation};
      result = H2_PAL_OK;
      break;
    }
  }
  portEXIT_CRITICAL(&s_esp_net_mux);
  return result;
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
      if (record->type != type || record->wrapping ||
          (wrapping && (record->tls_slot >= 0 || record->references != 0u))) {
        result = H2_PAL_ERR_INVALID_STATE;
      } else {
        ++record->references;
        record->wrapping = wrapping;
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

/* Also used when optional TLS/reaper initialization failed. Plain network
 * operations remain available. No SDK operation runs inside the critical
 * section, and a native fd cannot be freed while any operation still pins it.
 */
static void esp_net_socket_unpin(const esp_net_socket_lease_t* lease,
                                 int wrapping) {
  int close_fd = -1;
  portENTER_CRITICAL(&s_esp_net_mux);
  esp_net_socket_record_t* record = &s_esp_net_records[lease->index];
  if (record->used && record->generation == lease->generation) {
    if (wrapping) record->wrapping = 0;
    --record->references;
    if (record->closed && record->references == 0u &&
        s_esp_net_reaper_task == NULL) {
      close_fd = record->fd;
      record->used = 0;
    }
  }
  portEXIT_CRITICAL(&s_esp_net_mux);
  if (close_fd >= 0) (void)close(close_fd);
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
    if (!record->closed) result = H2_PAL_OK;
  }
  portEXIT_CRITICAL(&s_esp_net_mux);
  return result;
}

static void esp_net_close(void* user, h2_pal_net_socket_t fd) {
  (void)user;
  int close_fd = -1;
  portENTER_CRITICAL(&s_esp_net_mux);
  for (size_t i = 0u; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
    esp_net_socket_record_t* record = &s_esp_net_records[i];
    if (record->used && record->fd == fd) {
      record->closed = 1;
      if (record->references == 0u && s_esp_net_reaper_task == NULL) {
        close_fd = fd;
        record->used = 0;
      }
      break;
    }
  }
  portEXIT_CRITICAL(&s_esp_net_mux);
  /* Close requests cancellation only. It never releases a caller's job or
   * publishes INPUT_READY/ABANDONED while that caller may still copy or do I/O.
   */
  esp_net_tls_jobs_wake();
  esp_net_socket_kick();
  if (close_fd >= 0) (void)close(close_fd);
}

static void esp_net_socket_reaper(void* unused) {
  (void)unused;
  for (;;) {
    (void)xSemaphoreTake(s_esp_net_reaper_wake, portMAX_DELAY);
    for (size_t i = 0u; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
      int fd = -1, shutdown_fd = -1, slot_index = -1;
      uint32_t generation = 0u, tls_generation = 0u;
      portENTER_CRITICAL(&s_esp_net_mux);
      esp_net_socket_record_t* record = &s_esp_net_records[i];
      if (record->used && record->closed) {
        if (!record->shutdown_started) {
          record->shutdown_started = 1;
          ++record->references;
          shutdown_fd = record->fd;
        } else if (record->references == 0u) {
          fd = record->fd;
          slot_index = record->tls_slot;
          tls_generation = record->tls_generation;
        }
        generation = record->generation;
      }
      portEXIT_CRITICAL(&s_esp_net_mux);
      if (shutdown_fd >= 0) {
        /* Wake existing recv/accept/wait calls without releasing their fd. */
        (void)shutdown(shutdown_fd, SHUT_RDWR);
        esp_net_socket_lease_t lease = {i, generation, shutdown_fd};
        esp_net_socket_unpin(&lease, 0);
        continue;
      }
      if (fd < 0) continue;
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
      /* Detach TLS identity before native close can let a new open reuse fd.
       * No pins remain. After removing this record, only this reaper owns fd;
       * it never writes the old record again, even if its index is reused. */
      portENTER_CRITICAL(&s_esp_net_mux);
      record->used = 0;
      portEXIT_CRITICAL(&s_esp_net_mux);
      (void)close(fd);
      if (slot != NULL) {
        mbedtls_ssl_free(&slot->ssl);
        mbedtls_ssl_config_free(&slot->config);
        mbedtls_x509_crt_free(&slot->ca);
        (void)xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY);
        memset(slot, 0, sizeof(*slot));
        slot->fd = -1;
        ++s_esp_tls_sync->generations[slot_index];
        (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
      }
      if (slot_index >= 0) esp_net_tls_release((size_t)slot_index);
    }
  }
}

static int esp_net_socket_adopt(int result, int type,
                                h2_pal_net_socket_t* out) {
  if (result != H2_PAL_OK) return result;
  result = esp_net_socket_register(*out, type);
  if (result != H2_PAL_OK) {
    (void)close(*out);
    *out = -1;
  }
  return result;
}

static int esp_net_tcp_open_managed(void* user, h2_pal_net_family_t family,
                                    h2_pal_net_socket_t* out) {
  return esp_net_socket_adopt(esp_net_tcp_open(user, family, out), SOCK_STREAM,
                              out);
}
static int esp_net_tcp_open_bound_managed(void* user,
                                          h2_pal_net_family_t family,
                                          const h2_pal_net_bind_t* bind,
                                          h2_pal_net_socket_t* out) {
  return esp_net_socket_adopt(esp_net_tcp_open_bound(user, family, bind, out),
                              SOCK_STREAM, out);
}
static int esp_net_udp_open_managed(void* user, h2_pal_net_family_t family,
                                    uint16_t port, h2_pal_net_socket_t* out,
                                    h2_pal_net_addr_t* address) {
  return esp_net_socket_adopt(
      esp_net_udp_open(user, family, port, out, address), SOCK_DGRAM, out);
}
static int esp_net_udp_open_bound_managed(void* user,
                                          h2_pal_net_family_t family,
                                          uint16_t port,
                                          const h2_pal_net_bind_t* bind,
                                          h2_pal_net_socket_t* out,
                                          h2_pal_net_addr_t* address) {
  return esp_net_socket_adopt(
      esp_net_udp_open_bound(user, family, port, bind, out, address),
      SOCK_DGRAM, out);
}
static int esp_net_tcp_listen_managed(void* user, h2_pal_net_family_t family,
                                      uint16_t port,
                                      const h2_pal_net_bind_t* bind,
                                      h2_pal_net_socket_t* out,
                                      h2_pal_net_addr_t* address) {
  return esp_net_socket_adopt(
      esp_net_tcp_listen(user, family, port, bind, out, address), SOCK_STREAM,
      out);
}

#define H2_ESP_NET_LEASE_BEGIN(kind)                \
  esp_net_socket_lease_t lease;                     \
  int rc = esp_net_socket_pin(fd, kind, 0, &lease); \
  if (rc != H2_PAL_OK) return rc
#define H2_ESP_NET_LEASE_END(expression)                     \
  rc = (expression);                                         \
  if (!esp_net_socket_alive(&lease)) rc = H2_PAL_ERR_CLOSED; \
  esp_net_socket_unpin(&lease, 0);                           \
  return rc

static h2_pal_result_t esp_net_tcp_connect_managed(
    void* user, h2_pal_net_socket_t fd, const h2_pal_net_addr_t* address,
    uint32_t timeout_ms) {
  H2_ESP_NET_LEASE_BEGIN(SOCK_STREAM);
  H2_ESP_NET_LEASE_END(esp_net_tcp_connect(user, fd, address, timeout_ms));
}
static h2_pal_result_t esp_net_tcp_accept_managed(void* user,
                                                  h2_pal_net_socket_t fd,
                                                  h2_pal_net_socket_t* out,
                                                  h2_pal_net_addr_t* peer,
                                                  uint32_t timeout_ms) {
  H2_ESP_NET_LEASE_BEGIN(SOCK_STREAM);
  rc = esp_net_tcp_accept(user, fd, out, peer, timeout_ms);
  if (rc == H2_PAL_OK) {
    if (!esp_net_socket_alive(&lease)) {
      (void)close(*out);
      *out = -1;
      rc = H2_PAL_ERR_CLOSED;
    } else
      rc = esp_net_socket_adopt(rc, SOCK_STREAM, out);
  }
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
  H2_ESP_NET_LEASE_END(
      esp_net_udp_recvfrom(user, fd, address, data, length, timeout_ms));
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
  if (!esp_net_tls_available()) return H2_PAL_ERR_UNAVAILABLE;
  esp_net_socket_lease_t lease;
  int rc = esp_net_socket_pin(fd, SOCK_STREAM, 1, &lease);
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
