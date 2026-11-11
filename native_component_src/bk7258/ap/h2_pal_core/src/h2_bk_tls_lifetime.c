#include "h2_bk_tls_lifetime.h"

#include <limits.h>
#include <string.h>

#define H2_BK_TLS_WAIT_SLICE_MS 10u
#define H2_BK_TLS_COPY_CHUNK 256u

static bool lifetime_valid(const h2_bk_tls_lifetime_t* manager) {
  return manager != NULL && manager->initialized;
}

static void clear_ref(h2_bk_tls_tcp_ref_t* ref) {
  ref->generation = 0u;
  ref->index = H2_BK_TLS_TCP_MAX;
}

/* All helpers accessing lifecycle metadata require the supplied guard. */
static h2_bk_tls_tcp_record_t* find_tcp(h2_bk_tls_lifetime_t* manager, int fd) {
  for (size_t i = 0u; i < H2_BK_TLS_TCP_MAX; ++i) {
    if (manager->tcp[i].state != H2_BK_TLS_TCP_FREE &&
        manager->tcp[i].fd == fd) {
      return &manager->tcp[i];
    }
  }
  return NULL;
}

static h2_bk_tls_tcp_record_t* find_ref(h2_bk_tls_lifetime_t* manager,
                                        const h2_bk_tls_tcp_ref_t* ref) {
  if (ref->index >= H2_BK_TLS_TCP_MAX || ref->generation == 0u) return NULL;
  h2_bk_tls_tcp_record_t* tcp = &manager->tcp[ref->index];
  return tcp->state != H2_BK_TLS_TCP_FREE &&
                 tcp->generation == ref->generation && tcp->refs != 0u
             ? tcp
             : NULL;
}

/* Caller holds the guard. An idle claim excludes workers and fd registration
 * until native close has returned and that same generation is retired. */
static int claim_idle_close(h2_bk_tls_tcp_record_t* tcp) {
  if (tcp == NULL || tcp->state != H2_BK_TLS_TCP_CLOSED || !tcp->idle ||
      tcp->tls_owned || tcp->refs != 0u || tcp->reaping || tcp->fd < 0)
    return -1;
  tcp->reaping = true;
  return tcp->fd;
}

/* Only proven idle/listener closes may execute on the releasing caller. Native
 * connected close can wait for FIN and always belongs to the plain worker. */
static void finish_idle_close(h2_bk_tls_lifetime_t* manager,
                              const h2_bk_tls_tcp_ref_t* ref, int fd) {
  if (fd < 0) return;
  manager->ops.close_fd(manager->ops.user, fd);
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_bk_tls_tcp_record_t* tcp = &manager->tcp[ref->index];
  if (tcp->generation == ref->generation && tcp->fd == fd &&
      tcp->state == H2_BK_TLS_TCP_CLOSED && tcp->reaping)
    *tcp = (h2_bk_tls_tcp_record_t){.fd = -1, .job = -1};
  manager->ops.leave(manager->ops.user, guard);
}

static void abandon_job(h2_bk_tls_tcp_record_t* tcp, h2_bk_tls_job_t* job,
                        h2_pal_result_t result) {
  job->abandoned = true;
  job->result = result;
  if (tcp->caller_owned) tcp->state = H2_BK_TLS_TCP_FAILED;
}

h2_pal_result_t h2_bk_tls_lifetime_init(h2_bk_tls_lifetime_t* manager,
                                        const h2_bk_tls_lifetime_ops_t* ops) {
  if (manager == NULL || ops == NULL || ops->enter == NULL ||
      ops->leave == NULL || ops->clock_ms == NULL || ops->wait_ms == NULL ||
      ops->wake_worker == NULL || ops->wrap == NULL || ops->dispose == NULL ||
      ops->close_fd == NULL ||
      ((ops->alloc_root == NULL) != (ops->free_root == NULL))) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(manager, 0, sizeof(*manager));
  manager->ops = *ops;
  for (size_t i = 0u; i < H2_BK_TLS_TCP_MAX; ++i) {
    manager->tcp[i].fd = -1;
    manager->tcp[i].job = -1;
  }
  manager->initialized = true;
  return H2_PAL_OK;
}

void h2_bk_tls_lifetime_set_worker_ready(h2_bk_tls_lifetime_t* manager,
                                         bool ready) {
  if (!lifetime_valid(manager)) return;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  manager->worker_ready = ready;
  manager->ops.leave(manager->ops.user, guard);
}

h2_pal_result_t h2_bk_tls_tcp_register(h2_bk_tls_lifetime_t* manager, int fd) {
  if (!lifetime_valid(manager) || fd < 0) return H2_PAL_ERR_INVALID_ARG;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_pal_result_t result = H2_PAL_ERR_NO_SPACE;
  if (find_tcp(manager, fd) != NULL) {
    result = H2_PAL_ERR_INVALID_STATE;
  } else {
    for (size_t i = 0u; i < H2_BK_TLS_TCP_MAX; ++i) {
      h2_bk_tls_tcp_record_t* tcp = &manager->tcp[i];
      if (tcp->state != H2_BK_TLS_TCP_FREE) continue;
      if (++manager->generation == 0u) ++manager->generation;
      *tcp = (h2_bk_tls_tcp_record_t){.fd = fd,
                                      .generation = manager->generation,
                                      .job = -1,
                                      .state = H2_BK_TLS_TCP_PLAIN,
                                      .caller_owned = true};
      result = H2_PAL_OK;
      break;
    }
  }
  manager->ops.leave(manager->ops.user, guard);
  return result;
}

h2_pal_result_t h2_bk_tls_tcp_mark_idle(h2_bk_tls_lifetime_t* manager, int fd) {
  if (!lifetime_valid(manager)) return H2_PAL_ERR_INVALID_ARG;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_bk_tls_tcp_record_t* tcp = find_tcp(manager, fd);
  h2_pal_result_t result = H2_PAL_ERR_INVALID_STATE;
  if (tcp != NULL && tcp->caller_owned && tcp->refs == 0u &&
      tcp->state == H2_BK_TLS_TCP_PLAIN && !tcp->tls_owned) {
    tcp->idle = true;
    result = H2_PAL_OK;
  }
  manager->ops.leave(manager->ops.user, guard);
  return result;
}

h2_pal_result_t h2_bk_tls_tcp_connect_begin(h2_bk_tls_lifetime_t* manager,
                                            const h2_bk_tls_tcp_ref_t* ref) {
  if (!lifetime_valid(manager) || ref == NULL) return H2_PAL_ERR_INVALID_ARG;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_bk_tls_tcp_record_t* tcp = find_ref(manager, ref);
  h2_pal_result_t result = H2_PAL_ERR_CLOSED;
  if (tcp != NULL && tcp->caller_owned && tcp->state == H2_BK_TLS_TCP_PLAIN) {
    tcp->idle = false;
    result = H2_PAL_OK;
  }
  manager->ops.leave(manager->ops.user, guard);
  return result;
}

h2_pal_result_t h2_bk_tls_accept_begin(h2_bk_tls_lifetime_t* manager, int fd,
                                       h2_bk_tls_tcp_ref_t* listener_ref,
                                       h2_bk_tls_tcp_ref_t* accepted_ref) {
  if (listener_ref != NULL) clear_ref(listener_ref);
  if (accepted_ref != NULL) clear_ref(accepted_ref);
  if (!lifetime_valid(manager) || listener_ref == NULL || accepted_ref == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_bk_tls_tcp_record_t* listener = find_tcp(manager, fd);
  h2_pal_result_t result = H2_PAL_ERR_CLOSED;
  if (listener != NULL && listener->caller_owned &&
      listener->state == H2_BK_TLS_TCP_PLAIN) {
    result = H2_PAL_ERR_NO_SPACE;
    if (listener->refs != UINT32_MAX) {
      for (size_t i = 0u; i < H2_BK_TLS_TCP_MAX; ++i) {
        h2_bk_tls_tcp_record_t* accepted = &manager->tcp[i];
        if (accepted->state != H2_BK_TLS_TCP_FREE) continue;
        if (++manager->generation == 0u) ++manager->generation;
        *accepted =
            (h2_bk_tls_tcp_record_t){.fd = -1,
                                     .generation = manager->generation,
                                     .refs = 1u,
                                     .job = -1,
                                     .state = H2_BK_TLS_TCP_ACCEPT_RESERVED};
        ++listener->refs;
        *listener_ref =
            (h2_bk_tls_tcp_ref_t){.generation = listener->generation,
                                  .index = (size_t)(listener - manager->tcp)};
        *accepted_ref = (h2_bk_tls_tcp_ref_t){
            .generation = accepted->generation, .index = i};
        result = H2_PAL_OK;
        break;
      }
    }
  }
  manager->ops.leave(manager->ops.user, guard);
  return result;
}

h2_pal_result_t h2_bk_tls_accept_finish(h2_bk_tls_lifetime_t* manager,
                                        h2_bk_tls_tcp_ref_t* listener_ref,
                                        h2_bk_tls_tcp_ref_t* accepted_ref,
                                        int fd, h2_pal_result_t result,
                                        h2_pal_net_socket_t* out_socket) {
  if (out_socket != NULL) *out_socket = -1;
  if (!lifetime_valid(manager) || listener_ref == NULL ||
      accepted_ref == NULL || out_socket == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_bk_tls_tcp_record_t* listener = find_ref(manager, listener_ref);
  h2_bk_tls_tcp_record_t* accepted = find_ref(manager, accepted_ref);
  if (listener == NULL || accepted == NULL ||
      accepted->state != H2_BK_TLS_TCP_ACCEPT_RESERVED) {
    manager->ops.leave(manager->ops.user, guard);
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (!listener->caller_owned) result = H2_PAL_ERR_CLOSED;
  /* A native close can release its fd just before retiring old metadata.
   * Never publish a second generation under that still-occupied fd number. */
  if (fd >= 0 && result == H2_PAL_OK && find_tcp(manager, fd) != NULL)
    result = H2_PAL_ERR_INVALID_STATE;
  if (fd >= 0) {
    accepted->fd = fd;
    accepted->refs = 0u;
    accepted->caller_owned = result == H2_PAL_OK;
    accepted->state =
        result == H2_PAL_OK ? H2_BK_TLS_TCP_PLAIN : H2_BK_TLS_TCP_CLOSED;
    if (result == H2_PAL_OK) *out_socket = fd;
  } else {
    *accepted = (h2_bk_tls_tcp_record_t){.fd = -1, .job = -1};
  }
  --listener->refs;
  h2_bk_tls_tcp_ref_t idle_ref = *listener_ref;
  int idle_fd = claim_idle_close(listener);
  clear_ref(listener_ref);
  clear_ref(accepted_ref);
  manager->ops.leave(manager->ops.user, guard);
  finish_idle_close(manager, &idle_ref, idle_fd);
  manager->ops.wake_worker(manager->ops.user);
  return result;
}

h2_pal_result_t h2_bk_tls_tcp_acquire(h2_bk_tls_lifetime_t* manager, int fd,
                                      h2_bk_tls_tcp_ref_t* out_ref,
                                      bool* out_tls) {
  if (out_ref != NULL) clear_ref(out_ref);
  if (out_tls != NULL) *out_tls = false;
  if (!lifetime_valid(manager) || fd < 0 || out_ref == NULL || out_tls == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_bk_tls_tcp_record_t* tcp = find_tcp(manager, fd);
  h2_pal_result_t result = H2_PAL_ERR_NOT_FOUND;
  if (tcp != NULL) {
    result = H2_PAL_ERR_CLOSED;
    if (tcp->caller_owned && (tcp->state == H2_BK_TLS_TCP_PLAIN ||
                              tcp->state == H2_BK_TLS_TCP_ACTIVE)) {
      result = H2_PAL_ERR_NO_SPACE;
      if (tcp->refs != UINT32_MAX) {
        ++tcp->refs;
        out_ref->index = (size_t)(tcp - manager->tcp);
        out_ref->generation = tcp->generation;
        *out_tls = tcp->state == H2_BK_TLS_TCP_ACTIVE;
        result = H2_PAL_OK;
      }
    }
  }
  manager->ops.leave(manager->ops.user, guard);
  return result;
}

h2_pal_result_t h2_bk_tls_tcp_release(h2_bk_tls_lifetime_t* manager,
                                      h2_bk_tls_tcp_ref_t* ref) {
  if (!lifetime_valid(manager) || ref == NULL) return H2_PAL_ERR_INVALID_ARG;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_bk_tls_tcp_record_t* tcp = find_ref(manager, ref);
  h2_pal_result_t result = H2_PAL_ERR_INVALID_STATE;
  h2_bk_tls_tcp_ref_t idle_ref = *ref;
  int idle_fd = -1;
  if (tcp != NULL && tcp->state != H2_BK_TLS_TCP_LEGACY) {
    --tcp->refs;
    idle_fd = claim_idle_close(tcp);
    clear_ref(ref);
    result = H2_PAL_OK;
  }
  manager->ops.leave(manager->ops.user, guard);
  finish_idle_close(manager, &idle_ref, idle_fd);
  if (result == H2_PAL_OK) manager->ops.wake_worker(manager->ops.user);
  return result;
}

h2_pal_result_t h2_bk_tls_legacy_begin(h2_bk_tls_lifetime_t* manager, int fd,
                                       h2_bk_tls_tcp_ref_t* out_ref) {
  if (out_ref != NULL) clear_ref(out_ref);
  if (!lifetime_valid(manager) || fd < 0 || out_ref == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_bk_tls_tcp_record_t* tcp = find_tcp(manager, fd);
  h2_pal_result_t result = H2_PAL_ERR_NOT_FOUND;
  if (tcp != NULL) {
    result = H2_PAL_ERR_INVALID_STATE;
    if (tcp->caller_owned && tcp->state == H2_BK_TLS_TCP_PLAIN) {
      result = H2_PAL_ERR_BUSY;
      if (tcp->refs == 0u) {
        tcp->state = H2_BK_TLS_TCP_LEGACY;
        tcp->tls_owned = true;
        tcp->refs = 1u;
        out_ref->index = (size_t)(tcp - manager->tcp);
        out_ref->generation = tcp->generation;
        result = H2_PAL_OK;
      }
    }
  }
  manager->ops.leave(manager->ops.user, guard);
  return result;
}

h2_pal_result_t h2_bk_tls_legacy_finish(h2_bk_tls_lifetime_t* manager,
                                        h2_bk_tls_tcp_ref_t* ref,
                                        h2_pal_result_t result) {
  if (!lifetime_valid(manager) || ref == NULL) return H2_PAL_ERR_INVALID_ARG;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_bk_tls_tcp_record_t* tcp = find_ref(manager, ref);
  if (tcp == NULL || tcp->job >= 0 ||
      (tcp->state != H2_BK_TLS_TCP_LEGACY &&
       tcp->state != H2_BK_TLS_TCP_CLOSED)) {
    manager->ops.leave(manager->ops.user, guard);
    return H2_PAL_ERR_INVALID_STATE;
  }
  tcp->context_present = true;
  --tcp->refs;
  if (!tcp->caller_owned) {
    result = H2_PAL_ERR_CLOSED;
  } else {
    tcp->state =
        result == H2_PAL_OK ? H2_BK_TLS_TCP_ACTIVE : H2_BK_TLS_TCP_FAILED;
  }
  clear_ref(ref);
  manager->ops.leave(manager->ops.user, guard);
  manager->ops.wake_worker(manager->ops.user);
  return result;
}

h2_pal_result_t h2_bk_tls_tcp_close(h2_bk_tls_lifetime_t* manager, int fd) {
  if (!lifetime_valid(manager) || fd < 0) return H2_PAL_ERR_INVALID_ARG;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_bk_tls_tcp_record_t* tcp = find_tcp(manager, fd);
  h2_bk_tls_tcp_ref_t idle_ref = {0};
  int idle_fd = -1;
  if (tcp != NULL) {
    tcp->caller_owned = false;
    tcp->state = H2_BK_TLS_TCP_CLOSED;
    if (tcp->job >= 0)
      abandon_job(tcp, &manager->jobs[tcp->job], H2_PAL_ERR_CLOSED);
    idle_ref = (h2_bk_tls_tcp_ref_t){.generation = tcp->generation,
                                     .index = (size_t)(tcp - manager->tcp)};
    idle_fd = claim_idle_close(tcp);
  }
  manager->ops.leave(manager->ops.user, guard);
  if (tcp == NULL) return H2_PAL_ERR_NOT_FOUND;
  finish_idle_close(manager, &idle_ref, idle_fd);
  manager->ops.wake_worker(manager->ops.user);
  return H2_PAL_OK;
}

bool h2_bk_tls_plain_worker_step(h2_bk_tls_lifetime_t* manager) {
  if (!lifetime_valid(manager)) return false;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  for (size_t i = 0u; i < H2_BK_TLS_TCP_MAX; ++i) {
    h2_bk_tls_tcp_record_t* tcp = &manager->tcp[i];
    if (tcp->state != H2_BK_TLS_TCP_CLOSED || tcp->tls_owned ||
        tcp->refs != 0u || tcp->reaping)
      continue;
    tcp->reaping = true;
    int fd = tcp->fd;
    manager->ops.leave(manager->ops.user, guard);
    manager->ops.close_fd(manager->ops.user, fd);
    guard = manager->ops.enter(manager->ops.user);
    *tcp = (h2_bk_tls_tcp_record_t){.fd = -1, .job = -1};
    manager->ops.leave(manager->ops.user, guard);
    return true;
  }
  manager->ops.leave(manager->ops.user, guard);
  return false;
}

bool h2_bk_tls_bio_canceled(h2_bk_tls_lifetime_t* manager, int fd) {
  if (!lifetime_valid(manager) || fd < 0) return true;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_bk_tls_tcp_record_t* tcp = find_tcp(manager, fd);
  bool canceled =
      tcp == NULL || !tcp->caller_owned || tcp->state == H2_BK_TLS_TCP_FAILED;
  if (!canceled && tcp->job >= 0) {
    h2_bk_tls_job_t* job = &manager->jobs[tcp->job];
    canceled = job->abandoned ||
               (job->state != H2_BK_TLS_JOB_ACTIVE &&
                manager->ops.clock_ms(manager->ops.user) >= job->deadline_ms);
  }
  manager->ops.leave(manager->ops.user, guard);
  return canceled;
}

static h2_pal_result_t config_string_length(h2_bk_tls_lifetime_t* manager,
                                            const char* value, size_t capacity,
                                            uint64_t deadline_ms,
                                            size_t* out_length) {
  for (size_t i = 0u; i < capacity; ++i) {
    if (manager->ops.clock_ms(manager->ops.user) >= deadline_ms)
      return H2_PAL_ERR_TIMEOUT;
    if (value[i] == '\0') {
      *out_length = i;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_INVALID_ARG;
}

/* Caller holds the metadata guard and the preparation lifetime pin. */
static h2_pal_result_t preparation_status(h2_bk_tls_lifetime_t* manager,
                                          h2_bk_tls_tcp_record_t* tcp,
                                          const h2_bk_tls_job_t* job) {
  if (!tcp->caller_owned) return H2_PAL_ERR_CLOSED;
  if (manager->ops.clock_ms(manager->ops.user) >= job->deadline_ms)
    return H2_PAL_ERR_TIMEOUT;
  if (job->abandoned || job->state == H2_BK_TLS_JOB_DONE) return job->result;
  return H2_PAL_OK;
}

static void abandon_preparation(h2_bk_tls_tcp_record_t* tcp,
                                h2_bk_tls_job_t* job, h2_pal_result_t result) {
  abandon_job(tcp, job, result);
  job->waiter = false;
  job->preparing = false;
  --tcp->refs;
  /* An allocator already in native code owns a separate pin and publishes its
   * eventual result without consulting the caller's borrowed input. */
  if (job->state != H2_BK_TLS_JOB_ALLOCATING) job->state = H2_BK_TLS_JOB_DONE;
}

static void wait_for_job(h2_bk_tls_lifetime_t* manager, uint64_t deadline_ms) {
  uint64_t now_ms = manager->ops.clock_ms(manager->ops.user);
  if (now_ms >= deadline_ms) return;
  uint64_t remaining_ms = deadline_ms - now_ms;
  uint32_t wait_ms = remaining_ms > H2_BK_TLS_WAIT_SLICE_MS
                         ? H2_BK_TLS_WAIT_SLICE_MS
                         : (uint32_t)remaining_ms;
  manager->ops.wait_ms(manager->ops.user, wait_ms);
}

static h2_pal_result_t prepare_job(h2_bk_tls_lifetime_t* manager,
                                   h2_bk_tls_tcp_record_t* tcp,
                                   h2_bk_tls_job_t* job,
                                   const h2_pal_net_tls_config_t* config,
                                   size_t hostname_len, size_t alpn_len) {
  bool dynamic = config->root_ca_pem_len > H2_BK_TLS_CA_CAPACITY;
  /* Publish only owned pointers. The allocation request contains a length,
   * never the borrowed PEM pointer; only this caller copies borrowed bytes. */
  job->config = (h2_pal_net_tls_config_t){
      .server_name = job->hostname,
      .alpn = alpn_len != 0u ? job->alpn[0] : NULL,
      .root_ca_pem = !dynamic && config->root_ca_pem_len != 0u ? job->ca : NULL,
      .root_ca_pem_len = config->root_ca_pem_len,
      .verify = config->verify};
  memcpy(job->hostname, config->server_name, hostname_len + 1u);
  if (alpn_len != 0u) memcpy(job->alpn[0], config->alpn, alpn_len + 1u);
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_pal_result_t result = preparation_status(manager, tcp, job);
  if (result != H2_PAL_OK) {
    manager->ops.leave(manager->ops.user, guard);
    return result;
  }
  if (dynamic)
    job->state = H2_BK_TLS_JOB_ALLOC_QUEUED;
  else
    job->state = H2_BK_TLS_JOB_COPYING;
  manager->ops.leave(manager->ops.user, guard);
  uint8_t* destination = job->ca;
  if (dynamic) {
    manager->ops.wake_worker(manager->ops.user);
    for (;;) {
      guard = manager->ops.enter(manager->ops.user);
      result = preparation_status(manager, tcp, job);
      if (result != H2_PAL_OK) {
        manager->ops.leave(manager->ops.user, guard);
        return result;
      }
      if (job->state == H2_BK_TLS_JOB_COPY_READY) {
        destination = job->dynamic_ca;
        job->state = H2_BK_TLS_JOB_COPYING;
        manager->ops.leave(manager->ops.user, guard);
        break;
      }
      manager->ops.leave(manager->ops.user, guard);
      wait_for_job(manager, job->deadline_ms);
    }
  }
  size_t offset = 0u;
  while (offset < config->root_ca_pem_len) {
    guard = manager->ops.enter(manager->ops.user);
    result = preparation_status(manager, tcp, job);
    manager->ops.leave(manager->ops.user, guard);
    if (result != H2_PAL_OK) return result;
    size_t count = config->root_ca_pem_len - offset;
    if (count > H2_BK_TLS_COPY_CHUNK) count = H2_BK_TLS_COPY_CHUNK;
    memcpy(destination + offset, config->root_ca_pem + offset, count);
    offset += count;
  }
  destination[config->root_ca_pem_len] = '\0';
  return H2_PAL_OK;
}

h2_pal_result_t h2_bk_tls_wrap_bounded(h2_bk_tls_lifetime_t* manager, int fd,
                                       const h2_pal_net_tls_config_t* config,
                                       uint32_t timeout_ms,
                                       h2_pal_net_socket_t* out_socket) {
  if (out_socket != NULL) *out_socket = -1;
  if (!lifetime_valid(manager) || fd < 0 || config == NULL ||
      out_socket == NULL || timeout_ms == 0u || config->server_name == NULL ||
      (config->root_ca_pem == NULL && config->root_ca_pem_len != 0u) ||
      config->root_ca_pem_len == SIZE_MAX ||
      (config->verify != H2_PAL_NET_TLS_VERIFY_DEFAULT &&
       config->verify != H2_PAL_NET_TLS_VERIFY_REQUIRED &&
       config->verify != H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  uint64_t started_ms = manager->ops.clock_ms(manager->ops.user);
  uint64_t deadline_ms = started_ms > UINT64_MAX - timeout_ms
                             ? UINT64_MAX
                             : started_ms + timeout_ms;
  if (config->root_ca_pem_len > H2_BK_TLS_CA_CAPACITY &&
      manager->ops.alloc_root == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  size_t hostname_len = 0u, alpn_len = 0u;
  h2_pal_result_t result =
      config_string_length(manager, config->server_name,
                           H2_BK_TLS_HOST_CAPACITY, deadline_ms, &hostname_len);
  if (result != H2_PAL_OK) return result;
  if (hostname_len == 0u) return H2_PAL_ERR_INVALID_ARG;
  if (config->alpn != NULL) {
    result = config_string_length(
        manager, config->alpn, H2_BK_TLS_ALPN_CAPACITY, deadline_ms, &alpn_len);
    if (result != H2_PAL_OK) return result;
  }

  uintptr_t guard = manager->ops.enter(manager->ops.user);
  h2_bk_tls_tcp_record_t* tcp = find_tcp(manager, fd);
  result = H2_PAL_ERR_NOT_FOUND;
  h2_bk_tls_job_t* job = NULL;
  if (!manager->worker_ready) {
    result = H2_PAL_ERR_UNSUPPORTED;
  } else if (manager->ops.clock_ms(manager->ops.user) >= deadline_ms) {
    result = H2_PAL_ERR_TIMEOUT;
  } else if (tcp != NULL) {
    result = H2_PAL_ERR_INVALID_STATE;
    if (tcp->caller_owned && tcp->state == H2_BK_TLS_TCP_PLAIN) {
      result = H2_PAL_ERR_BUSY;
      if (tcp->refs == 0u) {
        result = H2_PAL_ERR_NO_SPACE;
        for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX; ++i) {
          if (manager->jobs[i].state != H2_BK_TLS_JOB_FREE) continue;
          job = &manager->jobs[i];
          job->state = H2_BK_TLS_JOB_PREPARING;
          job->tcp_index = (size_t)(tcp - manager->tcp);
          job->deadline_ms = deadline_ms;
          job->result = H2_PAL_ERR_WOULD_BLOCK;
          job->waiter = true;
          job->abandoned = false;
          job->preparing = true;
          job->dynamic_ca = NULL;
          tcp->job = (int)i;
          tcp->state = H2_BK_TLS_TCP_CONFIGURING;
          tcp->tls_owned = true;
          tcp->refs =
              1u; /* Preparation/queued work owns a native lifetime pin. */
          result = H2_PAL_OK;
          break;
        }
      }
    }
  }
  manager->ops.leave(manager->ops.user, guard);
  if (job == NULL) return result;

  result = prepare_job(manager, tcp, job, config, hostname_len, alpn_len);
  guard = manager->ops.enter(manager->ops.user);
  h2_pal_result_t status = preparation_status(manager, tcp, job);
  if (status != H2_PAL_OK) result = status;
  if (result != H2_PAL_OK) {
    abandon_preparation(tcp, job, result);
    manager->ops.leave(manager->ops.user, guard);
    manager->ops.wake_worker(manager->ops.user);
    return result;
  }
  /* Transfer the preparation pin to queued native work without a gap. */
  job->preparing = false;
  job->state = H2_BK_TLS_JOB_QUEUED;
  manager->ops.leave(manager->ops.user, guard);
  manager->ops.wake_worker(manager->ops.user);

  for (;;) {
    guard = manager->ops.enter(manager->ops.user);
    uint64_t now_ms = manager->ops.clock_ms(manager->ops.user);
    if (!tcp->caller_owned) {
      result = H2_PAL_ERR_CLOSED;
    } else if (now_ms >= deadline_ms) {
      result = H2_PAL_ERR_TIMEOUT;
    } else if (job->state == H2_BK_TLS_JOB_DONE) {
      result = job->result;
    } else {
      manager->ops.leave(manager->ops.user, guard);
      uint64_t remaining_ms = deadline_ms - now_ms;
      uint32_t wait_ms = remaining_ms > H2_BK_TLS_WAIT_SLICE_MS
                             ? H2_BK_TLS_WAIT_SLICE_MS
                             : (uint32_t)remaining_ms;
      manager->ops.wait_ms(manager->ops.user, wait_ms);
      continue;
    }
    if (result == H2_PAL_OK && !job->abandoned) {
      tcp->state = H2_BK_TLS_TCP_ACTIVE;
      job->state = H2_BK_TLS_JOB_ACTIVE;
      *out_socket = fd;
    } else {
      abandon_job(tcp, job, result);
    }
    job->waiter = false;
    manager->ops.leave(manager->ops.user, guard);
    manager->ops.wake_worker(manager->ops.user);
    return result;
  }
}

bool h2_bk_tls_worker_step(h2_bk_tls_lifetime_t* manager) {
  if (!lifetime_valid(manager)) return false;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  uint64_t now_ms = manager->ops.clock_ms(manager->ops.user);
  for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX; ++i) {
    h2_bk_tls_job_t* job = &manager->jobs[i];
    if (job->state != H2_BK_TLS_JOB_ALLOC_QUEUED &&
        job->state != H2_BK_TLS_JOB_COPY_READY &&
        job->state != H2_BK_TLS_JOB_QUEUED && job->state != H2_BK_TLS_JOB_DONE)
      continue;
    h2_bk_tls_tcp_record_t* tcp = &manager->tcp[job->tcp_index];
    if (!tcp->caller_owned || job->abandoned || now_ms >= job->deadline_ms) {
      if (!job->abandoned)
        abandon_job(tcp, job,
                    tcp->caller_owned ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_CLOSED);
      if (job->state != H2_BK_TLS_JOB_DONE) {
        if (job->state == H2_BK_TLS_JOB_QUEUED) --tcp->refs;
        /* Allocation-queued/copy-ready jobs still hold the caller's prep pin;
         * only that caller may relinquish it and stop accessing the buffer. */
        job->state = H2_BK_TLS_JOB_DONE;
        manager->ops.leave(manager->ops.user, guard);
        return true;
      }
    }
  }

  /* Reap failed TLS without consuming caller ownership. Failed sockets keep
   * their fd/record/job tombstones until tcp_close, even after disposal. */
  for (size_t i = 0u; i < H2_BK_TLS_TCP_MAX; ++i) {
    h2_bk_tls_tcp_record_t* tcp = &manager->tcp[i];
    if (!tcp->context_present || tcp->refs != 0u || tcp->reaping ||
        (tcp->state != H2_BK_TLS_TCP_FAILED &&
         tcp->state != H2_BK_TLS_TCP_CLOSED))
      continue;
    tcp->reaping = true;
    ++tcp->refs;
    int fd = tcp->fd;
    manager->ops.leave(manager->ops.user, guard);
    manager->ops.dispose(manager->ops.user, fd);
    guard = manager->ops.enter(manager->ops.user);
    tcp->context_present = false;
    tcp->reaping = false;
    --tcp->refs;
    manager->ops.leave(manager->ops.user, guard);
    return true;
  }

  /* No allocator/free runs on a caller or under metadata protection. The
   * preparation and native-wrap pins, plus waiter ownership, exclude a caller
   * copying bytes or a worker still consuming config. Keep the fd pinned even
   * across a blocking free, including successful ACTIVE connections. */
  for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX; ++i) {
    h2_bk_tls_job_t* job = &manager->jobs[i];
    if (job->dynamic_ca == NULL || job->preparing || job->waiter ||
        (job->state != H2_BK_TLS_JOB_DONE &&
         job->state != H2_BK_TLS_JOB_ACTIVE))
      continue;
    h2_bk_tls_tcp_record_t* tcp = &manager->tcp[job->tcp_index];
    if (tcp->refs != 0u || tcp->reaping) continue;
    tcp->reaping = true;
    ++tcp->refs;
    uint8_t* root = job->dynamic_ca;
    manager->ops.leave(manager->ops.user, guard);
    manager->ops.free_root(manager->ops.user, root);
    guard = manager->ops.enter(manager->ops.user);
    job->dynamic_ca = NULL;
    job->config.root_ca_pem = NULL;
    tcp->reaping = false;
    --tcp->refs;
    manager->ops.leave(manager->ops.user, guard);
    return true;
  }

  for (size_t i = 0u; i < H2_BK_TLS_TCP_MAX; ++i) {
    h2_bk_tls_tcp_record_t* tcp = &manager->tcp[i];
    if (tcp->state != H2_BK_TLS_TCP_CLOSED || !tcp->tls_owned ||
        tcp->refs != 0u || tcp->context_present || tcp->reaping)
      continue;
    h2_bk_tls_job_t* job = tcp->job >= 0 ? &manager->jobs[tcp->job] : NULL;
    if (job != NULL &&
        (job->waiter || job->preparing || job->dynamic_ca != NULL))
      continue;
    tcp->reaping = true;
    int fd = tcp->fd;
    manager->ops.leave(manager->ops.user, guard);
    manager->ops.close_fd(manager->ops.user, fd);
    guard = manager->ops.enter(manager->ops.user);
    if (job != NULL) job->state = H2_BK_TLS_JOB_FREE;
    *tcp = (h2_bk_tls_tcp_record_t){.fd = -1, .job = -1};
    manager->ops.leave(manager->ops.user, guard);
    return true;
  }

  for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX; ++i) {
    h2_bk_tls_job_t* job = &manager->jobs[i];
    if (job->state != H2_BK_TLS_JOB_ALLOC_QUEUED) continue;
    h2_bk_tls_tcp_record_t* tcp = &manager->tcp[job->tcp_index];
    if (manager->ops.clock_ms(manager->ops.user) >= job->deadline_ms) {
      abandon_job(tcp, job, H2_PAL_ERR_TIMEOUT);
      job->state = H2_BK_TLS_JOB_DONE;
      manager->ops.leave(manager->ops.user, guard);
      return true;
    }
    job->state = H2_BK_TLS_JOB_ALLOCATING;
    ++tcp->refs; /* Allocator owns a pin independent of caller preparation. */
    size_t bytes = job->config.root_ca_pem_len + 1u;
    manager->ops.leave(manager->ops.user, guard);
    uint8_t* root = manager->ops.alloc_root(manager->ops.user, bytes);
    guard = manager->ops.enter(manager->ops.user);
    job->dynamic_ca = root;
    job->config.root_ca_pem = root;
    --tcp->refs;
    if (!tcp->caller_owned) {
      abandon_job(tcp, job, H2_PAL_ERR_CLOSED);
      job->state = H2_BK_TLS_JOB_DONE;
    } else if (job->abandoned ||
               manager->ops.clock_ms(manager->ops.user) >= job->deadline_ms) {
      abandon_job(tcp, job, H2_PAL_ERR_TIMEOUT);
      job->state = H2_BK_TLS_JOB_DONE;
    } else if (root == NULL) {
      job->result = H2_PAL_ERR_NO_MEMORY;
      job->state = H2_BK_TLS_JOB_DONE;
      tcp->state = H2_BK_TLS_TCP_FAILED;
    } else {
      job->state = H2_BK_TLS_JOB_COPY_READY;
    }
    manager->ops.leave(manager->ops.user, guard);
    manager->ops.wake_worker(manager->ops.user);
    return true;
  }

  for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX; ++i) {
    h2_bk_tls_job_t* job = &manager->jobs[i];
    if (job->state != H2_BK_TLS_JOB_QUEUED) continue;
    h2_bk_tls_tcp_record_t* tcp = &manager->tcp[job->tcp_index];
    if (manager->ops.clock_ms(manager->ops.user) >= job->deadline_ms) {
      abandon_job(tcp, job, H2_PAL_ERR_TIMEOUT);
      job->state = H2_BK_TLS_JOB_DONE;
      --tcp->refs;
      manager->ops.leave(manager->ops.user, guard);
      return true;
    }
    job->state = H2_BK_TLS_JOB_RUNNING;
    tcp->context_present = true;
    int fd = tcp->fd;
    manager->ops.leave(manager->ops.user, guard);
    h2_pal_result_t result = manager->ops.wrap(manager->ops.user, fd,
                                               &job->config, job->deadline_ms);
    guard = manager->ops.enter(manager->ops.user);
    --tcp->refs;
    job->state = H2_BK_TLS_JOB_DONE;
    if (!tcp->caller_owned) {
      abandon_job(tcp, job, H2_PAL_ERR_CLOSED);
    } else if (job->abandoned ||
               manager->ops.clock_ms(manager->ops.user) >= job->deadline_ms) {
      abandon_job(tcp, job, H2_PAL_ERR_TIMEOUT);
    } else {
      job->result = result;
      if (result != H2_PAL_OK) tcp->state = H2_BK_TLS_TCP_FAILED;
    }
    manager->ops.leave(manager->ops.user, guard);
    manager->ops.wake_worker(manager->ops.user);
    return true;
  }
  manager->ops.leave(manager->ops.user, guard);
  return false;
}

uint32_t h2_bk_tls_worker_wait_ms(h2_bk_tls_lifetime_t* manager) {
  if (!lifetime_valid(manager)) return UINT32_MAX;
  uintptr_t guard = manager->ops.enter(manager->ops.user);
  uint64_t now_ms = manager->ops.clock_ms(manager->ops.user);
  uint32_t wait_ms = UINT32_MAX;
  for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX; ++i) {
    h2_bk_tls_job_t* job = &manager->jobs[i];
    if (!job->waiter || job->abandoned ||
        (job->state != H2_BK_TLS_JOB_COPY_READY &&
         (job->state != H2_BK_TLS_JOB_DONE || job->result != H2_PAL_OK)))
      continue;
    uint64_t remaining_ms =
        job->deadline_ms > now_ms ? job->deadline_ms - now_ms : 0u;
    if (remaining_ms >= UINT32_MAX) remaining_ms = UINT32_MAX - 1u;
    if (remaining_ms < wait_ms) wait_ms = (uint32_t)remaining_ms;
  }
  manager->ops.leave(manager->ops.user, guard);
  return wait_ms;
}
