/* Compiled with the production Net socket operations and TLS implementation. */
static void test_cancel_without_reaper(void);
static const h2_pal_net_tls_config_t test_config = {
    .server_name = "h2.example",
    .alpn = "h2",
    .root_ca_pem = (const uint8_t*)"ROOT",
    .root_ca_pem_len = 4u,
    .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
};
typedef struct test_call {
  int fd, legacy, result, done;
  uint32_t timeout_ms;
  h2_pal_net_socket_t out;
  const h2_pal_net_tls_config_t* config;
} test_call_t;

static void test_pause_ms(unsigned ms) {
  struct timespec pause = {ms / 1000u, (long)(ms % 1000u) * 1000000};
  assert(nanosleep(&pause, NULL) == 0);
}
static void test_gate_begin(int stage) {
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_stage = stage;
  test_gate_entered = test_gate_release = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
}
static void test_gate_wait(void) {
  assert(pthread_mutex_lock(&test_lock) == 0);
  struct timespec until = test_after_ms(3000u);
  while (!test_gate_entered)
    assert(pthread_cond_timedwait(&test_changed, &test_lock, &until) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
}
static void test_gate_end(void) {
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_stage = 0;
  test_gate_release = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
}
static void* test_call_wrap(void* user) {
  test_call_t* call = user;
  call->result =
      (call->legacy ? esp_net_tls_wrap_managed : esp_net_tls_wrap_bounded)(
          NULL, call->fd, call->config, call->timeout_ms, &call->out);
  assert(pthread_mutex_lock(&test_lock) == 0);
  call->done = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return NULL;
}
static void test_pair(int pair[2], int prime) {
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(esp_net_socket_register(pair[0], SOCK_STREAM) == H2_PAL_OK);
  if (prime) assert(send(pair[1], "WORLD", 5u, 0) == 5);
}
static void test_reuse_pair(int pair[2], int fd) {
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  if (pair[1] == fd) {
    int peer = dup(pair[1]);
    assert(peer >= 0);
    assert(close(pair[1]) == 0);
    pair[1] = peer;
  }
  if (pair[0] != fd) {
    assert(dup2(pair[0], fd) == fd);
    assert(close(pair[0]) == 0);
    pair[0] = fd;
  }
  assert(esp_net_socket_register(pair[0], SOCK_STREAM) == H2_PAL_OK);
  assert(send(pair[1], "WORLD", 5u, 0) == 5);
}
static void test_wait_jobs(void) {
  uint64_t until = esp_net_now_ms() + 3000u;
  for (;;) {
    int busy = 0;
    portENTER_CRITICAL(&s_esp_net_mux);
    for (size_t i = 0; i < H2_ESP_NET_TLS_WORKER_MAX; ++i)
      busy |= s_esp_tls_workers[i].job != NULL;
    portEXIT_CRITICAL(&s_esp_net_mux);
    if (!busy) return;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
}
static void test_wait_idle(void) {
  test_wait_jobs();
  uint64_t until = esp_net_now_ms() + 3000u;
  for (;;) {
    int busy = 0;
    portENTER_CRITICAL(&s_esp_net_mux);
    for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS; ++i)
      busy |= s_esp_net_records[i].used;
    portEXIT_CRITICAL(&s_esp_net_mux);
    assert(xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY));
    for (size_t i = 0; i < H2_ESP_NET_TLS_SOCKET_MAX; ++i)
      busy |= s_esp_tls_sockets[i].state != ESP_NET_TLS_SOCKET_FREE;
    assert(xSemaphoreGive(s_esp_tls_sync->registry_mutex));
    assert(pthread_mutex_lock(&test_lock) == 0);
    busy |= test_contexts != 0;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    if (!busy) break;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  assert(pthread_mutex_lock(&test_lock) == 0);
  assert(test_allocations == 1);
  assert(pthread_mutex_unlock(&test_lock) == 0);
}
static void test_wait_closed(int fd) {
  uint64_t until = esp_net_now_ms() + 3000u;
  while (fcntl(fd, F_GETFD) >= 0) {
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  assert(errno == EBADF);
}
static void test_check_hello(int peer) {
  char hello[5];
  assert(recv(peer, hello, sizeof(hello), 0) == 5 &&
         memcmp(hello, "HELLO", 5u) == 0);
}
static void test_success(int pair[2], int legacy) {
  h2_pal_net_socket_t out = -1;
  assert((legacy ? esp_net_tls_wrap_managed : esp_net_tls_wrap_bounded)(
             NULL, pair[0], &test_config, 1000u, &out) == H2_PAL_OK);
  assert(out == pair[0]);
  test_check_hello(pair[1]);
  assert(esp_net_tcp_send_timeout_managed(NULL, out, (const uint8_t*)"DATA", 4u,
                                          100u) == 4);
  char data[4];
  assert(recv(pair[1], data, 4u, 0) == 4 && memcmp(data, "DATA", 4u) == 0);
  esp_net_close(NULL, out);
  assert(close(pair[1]) == 0);
}

static void test_initialization_failure_and_plain_network(void) {
  test_fail_task =
      1; /* Reaper creation fails before optional TLS workers start. */
  esp_net_tls_init();
  assert(!s_esp_tls_initialized && !s_esp_tls_pool_ready);
  test_cancel_without_reaper();
  h2_pal_net_socket_t plain_fd = -1;
  h2_pal_net_addr_t plain_address;
  assert(esp_net_tcp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, &plain_fd) ==
         H2_PAL_OK);
  esp_net_close(NULL, plain_fd);
  assert(esp_net_udp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, &plain_fd,
                                  &plain_address) == H2_PAL_OK);
  esp_net_close(NULL, plain_fd);
  int pair[2];
  test_pair(pair, 0);
  int allocations = test_caller_allocations, tasks = test_task_count,
      sems = test_sem_creates;
  h2_pal_net_socket_t out = 123;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 20u, &out) ==
         H2_PAL_ERR_UNAVAILABLE);
  assert(out == -1 && allocations == test_caller_allocations &&
         tasks == test_task_count && sems == test_sem_creates);
  const h2_pal_net_vtable_t legacy_vtable = {.tls_wrap =
                                                 esp_net_tls_wrap_managed};
  const h2_pal_net_api_t legacy_api = {.vtable = &legacy_vtable};
  out = 123;
  test_fail_task = 1; /* A second transient failure keeps this first ordinary
                         retry unavailable. */
  assert(h2_pal_net_tls_wrap(&legacy_api, pair[0], &test_config, 20u, &out) ==
         H2_PAL_ERR_UNAVAILABLE);
  assert(out == -1 && allocations == test_caller_allocations &&
         tasks == test_task_count && sems == test_sem_creates);
  assert(esp_net_tcp_send_managed(NULL, pair[0], (const uint8_t*)"PLAIN", 5u) ==
         5);
  char plain[5];
  assert(recv(pair[1], plain, 5u, 0) == 5);
  esp_net_close(NULL, pair[0]);
  assert(close(pair[1]) == 0);
  test_pair(pair, 1);
  /* Reuse the retained API; ordinary TLS must retry initialization itself. */
  assert(h2_pal_net_tls_wrap(&legacy_api, pair[0], &test_config, 1000u, &out) ==
         H2_PAL_OK);
  test_check_hello(pair[1]);
  esp_net_close(NULL, out);
  assert(close(pair[1]) == 0);
  assert(s_esp_tls_initialized && s_esp_tls_pool_ready && test_task_count == 4);
  test_wait_idle();
}

static void test_no_caller_sdk_startup(void) {
  for (int stage = 8; stage <= 9; ++stage) {
    test_gate_begin(stage);
    int allocations = test_caller_allocations, sems = test_sem_creates;
    int deletes = test_sem_deletes, tasks = test_task_count;
    int pair[2];
    test_pair(pair, 1);
    test_success(pair, 0);
    test_wait_idle();
    assert(test_gate_entered == 0);
    assert(allocations == test_caller_allocations && sems == test_sem_creates &&
           deletes == test_sem_deletes && tasks == test_task_count);
    test_gate_end();
  }
}

static void test_worker_allocation_budget(void) {
  test_gate_begin(7);
  int pair[2];
  test_pair(pair, 1);
  int allocations = test_caller_allocations, parses = test_ca_checks;
  h2_pal_net_socket_t out = 123;
  uint64_t start = esp_net_now_ms();
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 40u, &out) ==
         H2_PAL_ERR_TIMEOUT);
  assert(out == -1 && esp_net_now_ms() - start < 400u);
  test_gate_wait();
  assert(allocations == test_caller_allocations);
  esp_net_close(NULL, pair[0]);
  test_wait_closed(pair[0]);
  assert(close(pair[1]) == 0);
  test_gate_end();
  test_wait_idle();
  assert(parses == test_ca_checks);
  int failed[2];
  test_pair(failed, 1);
  test_alloc_fail = 1;
  assert(esp_net_tls_wrap_bounded(NULL, failed[0], &test_config, 1000u, &out) ==
         H2_PAL_ERR_NO_MEMORY);
  esp_net_close(NULL, failed[0]);
  assert(close(failed[1]) == 0);
  test_wait_idle();
}

static void test_large_ca(void) {
  const size_t length = 70u * 1024u;
  uint8_t* ca = malloc(length);
  assert(ca != NULL);
  memset(ca, ' ', length);
  memcpy(ca, "ROOT", 4u);
  h2_pal_net_tls_config_t config = test_config;
  config.root_ca_pem = ca;
  config.root_ca_pem_len = length;
  test_expected_ca_length = length;
  int worker_allocations = test_worker_allocations,
      caller_allocations = test_caller_allocations;
  int pair[2];
  test_pair(pair, 1);
  h2_pal_net_socket_t out = -1;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &config, 1000u, &out) ==
         H2_PAL_OK);
  test_check_hello(pair[1]);
  esp_net_close(NULL, out);
  assert(close(pair[1]) == 0);
  test_wait_idle();
  assert(test_worker_allocations == worker_allocations + 1);
  assert(test_caller_allocations == caller_allocations &&
         test_max_allocation == length + 1u);
  test_pair(pair, 1);
  assert(esp_net_tls_wrap_managed(NULL, pair[0], &config, 1000u, &out) ==
         H2_PAL_OK);
  test_check_hello(pair[1]);
  esp_net_close(NULL, out);
  assert(close(pair[1]) == 0);
  test_wait_idle();
  test_expected_ca_length = 4u;
  free(ca);
}

static void test_close_during_copy(void) {
  size_t length = 70u * 1024u;
  uint8_t* ca = malloc(length);
  assert(ca);
  memset(ca, ' ', length);
  memcpy(ca, "ROOT", 4u);
  h2_pal_net_tls_config_t config = test_config;
  config.root_ca_pem = ca;
  config.root_ca_pem_len = length;
  int pair[2];
  test_pair(pair, 1);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_copy_gate = 1;
  test_copy_entered = test_copy_release = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_call_t call = {.fd = pair[0], .timeout_ms = 1000u, .config = &config};
  pthread_t caller;
  assert(pthread_create(&caller, NULL, test_call_wrap, &call) == 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  while (!test_copy_entered)
    assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  uint64_t start = esp_net_now_ms();
  esp_net_close(NULL, pair[0]);
  assert(esp_net_now_ms() - start < 100u && fcntl(pair[0], F_GETFD) >= 0);
  /* Close cannot transfer final ownership while memcpy still has the owned CA.
   */
  assert(pthread_mutex_lock(&test_lock) == 0);
  assert(!call.done && test_allocations == 2);
  test_copy_release = 1;
  test_copy_gate = 0;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(pthread_join(caller, NULL) == 0);
  assert(call.result == H2_PAL_ERR_CLOSED && call.out == -1);
  memset(ca, 0xa5, length);
  free(ca);
  assert(close(pair[1]) == 0);
  test_wait_idle();
}

static void test_close_live_wrap_and_reuse(int stage) {
  h2_pal_net_tls_config_t* config = malloc(sizeof(*config));
  assert(config);
  *config = test_config;
  char *name = strdup("h2.example"), *alpn = strdup("h2"), *ca = strdup("ROOT");
  assert(name && alpn && ca);
  config->server_name = name;
  config->alpn = alpn;
  config->root_ca_pem = stage == 4 ? NULL : (uint8_t*)ca;
  config->root_ca_pem_len = stage == 4 ? 0u : 4u;
  int pair[2];
  test_pair(pair, 1);
  test_gate_begin(stage);
  test_call_t call = {.fd = pair[0], .timeout_ms = 1000u, .config = config};
  pthread_t caller;
  assert(pthread_create(&caller, NULL, test_call_wrap, &call) == 0);
  test_gate_wait();
  h2_pal_net_socket_t other = -1;
  /* Both entries and all ordinary I/O see the same exclusive wrapping lease. */
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 40u, &other) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(esp_net_tls_wrap_managed(NULL, pair[0], &test_config, 40u, &other) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(esp_net_tcp_send_timeout_managed(NULL, pair[0], (const uint8_t*)"BAD",
                                          3u, 10u) == H2_PAL_ERR_INVALID_STATE);
  uint64_t start = esp_net_now_ms();
  esp_net_close(NULL, pair[0]);
  assert(esp_net_now_ms() - start < 100u);
  assert(pthread_join(caller, NULL) == 0);
  assert(call.result == H2_PAL_ERR_CLOSED && call.out == -1);
  memset(name, 'x', 10u);
  memset(alpn, 'x', 2u);
  memset(ca, 'x', 4u);
  memset(config, 0xa5, sizeof(*config));
  free(name);
  free(alpn);
  free(ca);
  free(config);
  test_wait_closed(pair[0]);
  /* Keep the SDK gate closed while a new connection gets the old raw number. */
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_stage = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  int fresh[2];
  test_reuse_pair(fresh, pair[0]);
  h2_pal_net_socket_t tls = -1;
  assert(esp_net_tls_wrap_bounded(NULL, fresh[0], &test_config, 1000u, &tls) ==
         H2_PAL_OK);
  test_check_hello(fresh[1]);
  test_gate_end();
  test_wait_jobs();
  /* No stale publish or late BIO bytes can touch the replacement generation. */
  char byte;
  assert(recv(fresh[1], &byte, 1u, MSG_DONTWAIT) < 0 &&
         (errno == EAGAIN || errno == EWOULDBLOCK));
  assert(esp_net_tcp_send_timeout_managed(NULL, tls, (const uint8_t*)"NEW", 3u,
                                          100u) == 3);
  char data[3];
  assert(recv(fresh[1], data, 3u, 0) == 3 && memcmp(data, "NEW", 3u) == 0);
  esp_net_close(NULL, tls);
  assert(close(fresh[1]) == 0);
  assert(close(pair[1]) == 0);
  test_wait_idle();
}

static void test_close_while_bio_is_active(void) {
  int pair[2];
  test_pair(pair, 1);
  test_gate_begin(10);
  test_call_t call = {
      .fd = pair[0], .timeout_ms = 1000u, .config = &test_config};
  pthread_t caller;
  assert(pthread_create(&caller, NULL, test_call_wrap, &call) == 0);
  test_gate_wait();
  esp_net_close(NULL, pair[0]);
  /* Caller is still inside nonblocking BIO recv: native fd must stay pinned. */
  assert(fcntl(pair[0], F_GETFD) >= 0);
  int unrelated[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, unrelated) == 0);
  assert(unrelated[0] != pair[0] && unrelated[1] != pair[0]);
  h2_pal_net_socket_t out = 0;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 10u, &out) ==
         H2_PAL_ERR_CLOSED);
  test_gate_end();
  assert(pthread_join(caller, NULL) == 0);
  assert(call.result == H2_PAL_ERR_CLOSED && call.out == -1);
  assert(close(unrelated[0]) == 0 && close(unrelated[1]) == 0 &&
         close(pair[1]) == 0);
  test_wait_idle();
}

static void test_capacity_and_network_timeout(void) {
  test_gate_begin(1);
  for (unsigned i = 0u; i < H2_ESP_NET_TLS_WORKER_MAX; ++i) {
    int pair[2];
    test_pair(pair, 1);
    h2_pal_net_socket_t out = -1;
    assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 40u, &out) ==
           H2_PAL_ERR_TIMEOUT);
    esp_net_close(NULL, pair[0]);
    assert(close(pair[1]) == 0);
  }
  int pair[2];
  test_pair(pair, 1);
  h2_pal_net_socket_t out = -1;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 100u, &out) ==
         H2_PAL_ERR_NO_SPACE);
  esp_net_close(NULL, pair[0]);
  assert(close(pair[1]) == 0);
  test_gate_end();
  test_wait_idle();
  test_pair(pair, 0);
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 40u, &out) ==
         H2_PAL_ERR_TIMEOUT);
  esp_net_close(NULL, pair[0]);
  assert(close(pair[1]) == 0);
  test_wait_idle();
}

static void test_parse_failure_cleanup(void) {
  int pair[2];
  test_pair(pair, 1);
  h2_pal_net_socket_t out = 123;
  test_parse_fail = 1;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 1000u, &out) ==
         H2_PAL_ERR_FORMAT);
  assert(out == -1);
  esp_net_close(NULL, pair[0]);
  assert(close(pair[1]) == 0);
  test_wait_idle();
  test_parse_fail = 0;
}

static void test_registry_deadlines(void) {
  int pair[2];
  test_pair(pair, 1);
  h2_pal_net_socket_t out = -1;
  assert(xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY));
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 40u, &out) ==
         H2_PAL_ERR_TIMEOUT);
  esp_net_close(NULL, pair[0]);
  test_wait_closed(pair[0]);
  assert(xSemaphoreGive(s_esp_tls_sync->registry_mutex));
  assert(close(pair[1]) == 0);
  test_wait_idle();
  test_pair(pair, 1);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_publish_block = 1;
  test_publish_held = test_publish_release = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 100u, &out) ==
         H2_PAL_ERR_TIMEOUT);
  esp_net_close(NULL, pair[0]);
  test_wait_closed(pair[0]);
  assert(pthread_mutex_lock(&test_lock) == 0);
  assert(test_publish_held);
  test_publish_release = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(pthread_join(test_publish_thread, NULL) == 0);
  assert(close(pair[1]) == 0);
  test_wait_idle();
}

static void test_legacy_compatibility_and_close(void) {
  int pair[2];
  test_pair(pair, 1);
  test_gate_begin(2);
  test_call_t call = {
      .fd = pair[0], .legacy = 1, .timeout_ms = 20u, .config = &test_config};
  pthread_t caller;
  assert(pthread_create(&caller, NULL, test_call_wrap, &call) == 0);
  test_gate_wait();
  test_pause_ms(40u);
  assert(pthread_mutex_lock(&test_lock) == 0);
  assert(!call.done);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_gate_end();
  assert(pthread_join(caller, NULL) == 0);
  assert(call.result == H2_PAL_ERR_TIMEOUT);
  esp_net_close(NULL, pair[0]);
  assert(close(pair[1]) == 0);
  test_wait_idle();
  test_pair(pair, 1);
  test_success(pair, 1);
  test_wait_idle();
  test_pair(pair, 1);
  test_gate_begin(2);
  call = (test_call_t){
      .fd = pair[0], .legacy = 1, .timeout_ms = 1000u, .config = &test_config};
  assert(pthread_create(&caller, NULL, test_call_wrap, &call) == 0);
  test_gate_wait();
  h2_pal_net_socket_t out = -1;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 40u, &out) ==
         H2_PAL_ERR_INVALID_STATE);
  uint64_t start = esp_net_now_ms();
  esp_net_close(NULL, pair[0]);
  assert(esp_net_now_ms() - start < 100u && fcntl(pair[0], F_GETFD) >= 0);
  test_gate_end();
  assert(pthread_join(caller, NULL) == 0);
  assert(call.result == H2_PAL_ERR_CLOSED && call.out == -1);
  assert(close(pair[1]) == 0);
  test_wait_idle();
}

static void test_deferred_destructor_and_reuse(void) {
  int pair[2];
  test_pair(pair, 1);
  h2_pal_net_socket_t out = -1;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 1000u, &out) ==
         H2_PAL_OK);
  test_check_hello(pair[1]);
  test_wait_jobs();
  test_gate_begin(6);
  uint64_t start = esp_net_now_ms();
  esp_net_close(NULL, out);
  assert(esp_net_now_ms() - start < 100u);
  test_gate_wait();
  test_wait_closed(out);
  int fresh[2];
  test_reuse_pair(fresh, out);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_stage = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  /* Old TLS identity was detached before native close; raw I/O uses fresh fd.
   */
  assert(esp_net_tcp_send_managed(NULL, fresh[0], (const uint8_t*)"RAW", 3u) ==
         3);
  char raw[3];
  assert(recv(fresh[1], raw, 3u, 0) == 3 && memcmp(raw, "RAW", 3u) == 0);
  assert(esp_net_tls_wrap_bounded(NULL, fresh[0], &test_config, 1000u, &out) ==
         H2_PAL_OK);
  test_check_hello(fresh[1]);
  test_gate_end();
  esp_net_close(NULL, out);
  assert(close(fresh[1]) == 0 && close(pair[1]) == 0);
  test_wait_idle();
}

static void test_late_accepted_cleanup(void) {
  int pair[2];
  test_pair(pair, 1);
  test_gate_begin(11);
  h2_pal_net_socket_t out = -1;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 1000u, &out) ==
         H2_PAL_OK);
  test_check_hello(pair[1]);
  test_gate_wait();
  esp_net_close(NULL, out);
  test_wait_closed(out);
  uint64_t until = esp_net_now_ms() + 1000u;
  for (;;) {
    assert(xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY));
    int free_slot = s_esp_tls_sockets[0].state == ESP_NET_TLS_SOCKET_FREE;
    assert(xSemaphoreGive(s_esp_tls_sync->registry_mutex));
    if (free_slot) break;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_stage = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  int fresh[2];
  test_reuse_pair(fresh, out);
  assert(esp_net_tls_wrap_bounded(NULL, fresh[0], &test_config, 1000u, &out) ==
         H2_PAL_OK);
  test_check_hello(fresh[1]);
  test_gate_end();
  test_wait_jobs();
  assert(esp_net_tcp_send_timeout_managed(NULL, out, (const uint8_t*)"LATE", 4u,
                                          100u) == 4);
  char data[4];
  assert(recv(fresh[1], data, 4u, 0) == 4 && memcmp(data, "LATE", 4u) == 0);
  esp_net_close(NULL, out);
  assert(close(fresh[1]) == 0 && close(pair[1]) == 0);
  test_wait_idle();
}

static void* test_plain_recv(void* user) {
  test_call_t* call = user;
  uint8_t byte;
  call->result = esp_net_tcp_recv_managed(NULL, call->fd, &byte, 1u, 1000u);
  return NULL;
}
static void test_public_socket_operations(void) {
  h2_pal_net_socket_t listener = -1, client = -1, server = -1;
  h2_pal_net_addr_t address, peer;
  assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                    &listener, &address) == H2_PAL_OK);
  address.ip[0] = 127;
  address.ip[1] = address.ip[2] = 0;
  address.ip[3] = 1;
  assert(esp_net_tcp_open_bound_managed(NULL, H2_PAL_NET_FAMILY_IPV4, NULL,
                                        &client) == H2_PAL_OK);
  assert(esp_net_tcp_connect_managed(NULL, client, &address, 1000u) ==
         H2_PAL_OK);
  assert(esp_net_tcp_accept_managed(NULL, listener, &server, &peer, 1000u) ==
         H2_PAL_OK);
  assert((fcntl(listener, F_GETFL, 0) & O_NONBLOCK) != 0);
  assert((fcntl(server, F_GETFL, 0) & O_NONBLOCK) == 0);
  assert(esp_net_tcp_send_timeout_managed(NULL, client, (const uint8_t*)"PING",
                                          4u, 100u) == 4);
  uint8_t bytes[4];
  assert(esp_net_tcp_recv_managed(NULL, server, bytes, 4u, 100u) == 4 &&
         memcmp(bytes, "PING", 4u) == 0);
  assert(esp_net_tcp_send_managed(NULL, server, (const uint8_t*)"PONG", 4u) ==
         4);
  assert(esp_net_tcp_recv_managed(NULL, client, bytes, 4u, 100u) == 4 &&
         memcmp(bytes, "PONG", 4u) == 0);
  test_call_t call = {.fd = server};
  pthread_t receiver;
  assert(pthread_create(&receiver, NULL, test_plain_recv, &call) == 0);
  uint64_t until = esp_net_now_ms() + 1000u;
  for (;;) {
    int pinned = 0;
    portENTER_CRITICAL(&s_esp_net_mux);
    for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS; ++i)
      if (s_esp_net_records[i].used && s_esp_net_records[i].fd == server)
        pinned = s_esp_net_records[i].references != 0u;
    portEXIT_CRITICAL(&s_esp_net_mux);
    if (pinned) break;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  esp_net_close(NULL, server);
  assert(pthread_join(receiver, NULL) == 0);
  assert(call.result == H2_PAL_ERR_CLOSED);
  esp_net_close(NULL, client);
  esp_net_close(NULL, listener);
  test_wait_idle();
  h2_pal_net_socket_t udp1 = -1, udp2 = -1;
  h2_pal_net_addr_t to;
  assert(esp_net_udp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, &udp1,
                                  &address) == H2_PAL_OK);
  assert(esp_net_udp_open_bound_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                        &udp2, &to) == H2_PAL_OK);
  to.ip[0] = 127;
  to.ip[1] = to.ip[2] = 0;
  to.ip[3] = 1;
  assert(esp_net_udp_sendto_managed(NULL, udp1, &to, (const uint8_t*)"UDP!",
                                    4u) == 4);
  assert(esp_net_udp_recvfrom_managed(NULL, udp2, &peer, bytes, 4u, 100u) ==
             4 &&
         memcmp(bytes, "UDP!", 4u) == 0);
  assert(esp_net_udp_join_multicast_managed(NULL, udp1, &to) ==
         H2_PAL_ERR_INVALID_ARG);
  esp_net_close(NULL, udp1);
  esp_net_close(NULL, udp2);
  test_wait_idle();
}

static void test_wait_pinned(int fd) {
  uint64_t until = esp_net_now_ms() + 1000u;
  for (;;) {
    int pinned = 0;
    portENTER_CRITICAL(&s_esp_net_mux);
    for (size_t i = 0u; i < CONFIG_LWIP_MAX_SOCKETS; ++i)
      if (s_esp_net_records[i].used && s_esp_net_records[i].fd == fd)
        pinned = s_esp_net_records[i].references != 0u;
    portEXIT_CRITICAL(&s_esp_net_mux);
    if (pinned) return;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
}
static int test_call_done(test_call_t* call, uint32_t timeout_ms) {
  uint64_t until = esp_net_now_ms() + timeout_ms;
  for (;;) {
    assert(pthread_mutex_lock(&test_lock) == 0);
    int done = call->done;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    if (done) return 1;
    if (esp_net_now_ms() >= until) return 0;
    test_pause_ms(1u);
  }
}
static void* test_cancellable_receive(void* user) {
  test_call_t* call = user;
  if (call->legacy == 1)
    call->result =
        esp_net_tcp_accept_managed(NULL, call->fd, &call->out, NULL, 1000u);
  else if (call->legacy == 2) {
    uint8_t byte;
    call->result =
        esp_net_udp_recvfrom_managed(NULL, call->fd, NULL, &byte, 1u, 1000u);
  } else {
    uint8_t byte;
    call->result = esp_net_tcp_recv_managed(NULL, call->fd, &byte, 1u, 1000u);
  }
  assert(pthread_mutex_lock(&test_lock) == 0);
  call->done = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return NULL;
}

typedef struct test_completed_io {
  int fd, receive, result;
  uint8_t data[4];
} test_completed_io_t;
static void* test_do_completed_io(void* user) {
  test_completed_io_t* call = user;
  call->result =
      call->receive
          ? esp_net_tcp_recv_managed(NULL, call->fd, call->data, 4u, 1000u)
          : esp_net_tcp_send_timeout_managed(NULL, call->fd,
                                             (const uint8_t*)"SENT", 4u, 1000u);
  return NULL;
}
static void test_completed_io_survives_close(void) {
  for (int tls = 0; tls <= 1; ++tls) {
    for (int receive = 0; receive <= 1; ++receive) {
      int pair[2];
      test_pair(pair, tls);
      if (tls) {
        h2_pal_net_socket_t out = -1;
        assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 1000u,
                                        &out) == H2_PAL_OK);
        test_check_hello(pair[1]);
        test_wait_jobs();
      }
      if (receive) assert(send(pair[1], "RECV", 4u, 0) == 4);
      test_gate_begin(receive ? 13 : 12);
      test_completed_io_t call = {.fd = pair[0], .receive = receive};
      pthread_t thread;
      assert(pthread_create(&thread, NULL, test_do_completed_io, &call) == 0);
      test_gate_wait();
      /* The native syscall has already consumed/copied all four bytes. */
      esp_net_close(NULL, pair[0]);
      test_gate_end();
      assert(pthread_join(thread, NULL) == 0);
      assert(call.result == 4);
      if (receive)
        assert(memcmp(call.data, "RECV", 4u) == 0);
      else {
        char bytes[4];
        assert(recv(pair[1], bytes, 4u, 0) == 4 &&
               memcmp(bytes, "SENT", 4u) == 0);
      }
      assert(close(pair[1]) == 0);
      test_wait_idle();
    }
  }
}

static void test_idle_close_reopens_immediately(void) {
  test_gate_begin(14);
  esp_net_socket_kick();
  test_gate_wait();
  h2_pal_net_socket_t first = -1, second = -1;
  h2_pal_net_addr_t address, rebound;
  assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                    &first, &address) == H2_PAL_OK);
  esp_net_close(NULL, first);
  assert(fcntl(first, F_GETFD) < 0 && errno == EBADF);
  assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, address.port,
                                    NULL, &second, &rebound) == H2_PAL_OK);
  esp_net_close(NULL, second);
  for (size_t i = 0u; i < CONFIG_LWIP_MAX_SOCKETS + 2u; ++i) {
    assert(esp_net_tcp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, &first) ==
           H2_PAL_OK);
    esp_net_close(NULL, first);
    assert(fcntl(first, F_GETFD) < 0 && errno == EBADF);
  }
  test_gate_end();
  test_wait_idle();
}

static void test_unrelated_close_during_tls_destructor(void) {
  int tls_pair[2];
  test_pair(tls_pair, 1);
  h2_pal_net_socket_t tls = -1;
  assert(esp_net_tls_wrap_bounded(NULL, tls_pair[0], &test_config, 1000u,
                                  &tls) == H2_PAL_OK);
  test_check_hello(tls_pair[1]);
  test_wait_jobs();
  test_gate_begin(6);
  esp_net_close(NULL, tls);
  test_gate_wait();
  int plain[2];
  test_pair(plain, 0);
  test_call_t call = {.fd = plain[0]};
  pthread_t receiver;
  assert(pthread_create(&receiver, NULL, test_cancellable_receive, &call) == 0);
  test_wait_pinned(plain[0]);
  esp_net_close(NULL, plain[0]);
  assert(test_call_done(&call, 150u));
  assert(pthread_join(receiver, NULL) == 0 && call.result == H2_PAL_ERR_CLOSED);
  test_wait_closed(plain[0]);
  h2_pal_net_socket_t listener = -1;
  h2_pal_net_addr_t address;
  assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                    &listener, &address) == H2_PAL_OK);
  esp_net_close(NULL, listener);
  assert(fcntl(listener, F_GETFD) < 0 && errno == EBADF);
  assert(close(plain[1]) == 0 && close(tls_pair[1]) == 0);
  test_gate_end();
  test_wait_idle();
}

static void test_cancel_without_reaper(void) {
  for (int accept_call = 0; accept_call <= 2; ++accept_call) {
    int pair[2] = {-1, -1};
    h2_pal_net_socket_t fd = -1;
    if (accept_call == 1) {
      h2_pal_net_addr_t address;
      assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                        &fd, &address) == H2_PAL_OK);
      /* ESP lwIP FULLDUPLEX listener shutdown may leave accept/select asleep.
       */
      test_listener_shutdown_noop = 1;
    } else if (accept_call == 2) {
      h2_pal_net_addr_t address;
      assert(esp_net_udp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, &fd,
                                      &address) == H2_PAL_OK);
    } else {
      test_pair(pair, 0);
      fd = pair[0];
    }
    test_call_t call = {.fd = fd, .legacy = accept_call};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, test_cancellable_receive, &call) == 0);
    test_wait_pinned(fd);
    esp_net_close(NULL, fd);
    assert(test_call_done(&call, 150u));
    assert(pthread_join(thread, NULL) == 0 && call.result == H2_PAL_ERR_CLOSED);
    assert(fcntl(fd, F_GETFD) < 0 && errno == EBADF);
    if (!accept_call) assert(close(pair[1]) == 0);
    test_listener_shutdown_noop = 0;
  }
}

static void test_retained_legacy_api_retry(int sync_failure) {
  const h2_pal_net_vtable_t vtable = {.tls_wrap = esp_net_tls_wrap_managed};
  const h2_pal_net_api_t api = {.vtable = &vtable};
  if (sync_failure)
    test_alloc_fail = 1;
  else
    test_fail_task = 1;
  esp_net_tls_init();
  assert(!s_esp_tls_initialized);
  int pair[2];
  test_pair(pair, 1);
  h2_pal_net_socket_t out = 123;
  /* No getter or explicit initialization retry between failure and this call.
   */
  assert(h2_pal_net_tls_wrap(&api, pair[0], &test_config, 1000u, &out) ==
         H2_PAL_OK);
  assert(out == pair[0]);
  test_check_hello(pair[1]);
  esp_net_close(NULL, out);
  assert(close(pair[1]) == 0);
  test_wait_idle();
}

static void* test_call_close(void* user) {
  test_call_t* call = user;
  esp_net_close(NULL, call->fd);
  assert(pthread_mutex_lock(&test_lock) == 0);
  call->done = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return NULL;
}
static void test_failed_bounded_close_has_no_fin_wait(void) {
  int pair[2];
  test_pair(pair, 1);
  test_gate_begin(1);
  h2_pal_net_socket_t out = -1;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 40u, &out) ==
         H2_PAL_ERR_TIMEOUT);
  test_gate_wait();
  test_gate_begin(15);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_simulate_fin_wait = 1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_call_t call = {.fd = pair[0]};
  pthread_t thread;
  assert(pthread_create(&thread, NULL, test_call_close, &call) == 0);
  assert(test_call_done(&call, 150u));
  assert(pthread_join(thread, NULL) == 0);
  assert(fcntl(pair[0], F_GETFD) < 0 && errno == EBADF);
  assert(pthread_mutex_lock(&test_lock) == 0);
  assert(test_gate_entered == 0);
  test_simulate_fin_wait = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_gate_end();
  assert(close(pair[1]) == 0);
  test_wait_idle();
}
static void test_graceful_close_profiles(void) {
  for (int mode = 0; mode <= 2; ++mode) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(address);
    assert(bind(listener, (struct sockaddr*)&address, length) == 0 &&
           listen(listener, 1) == 0);
    assert(getsockname(listener, (struct sockaddr*)&address, &length) == 0);
    h2_pal_net_socket_t fd = -1;
    assert(esp_net_tcp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, &fd) ==
           H2_PAL_OK);
    h2_pal_net_addr_t remote = {.family = H2_PAL_NET_FAMILY_IPV4,
                                .port = ntohs(address.sin_port),
                                .ip = {127, 0, 0, 1}};
    assert(esp_net_tcp_connect_managed(NULL, fd, &remote, 1000u) == H2_PAL_OK);
    int peer = accept(listener, NULL, NULL);
    assert(peer >= 0);
    assert(close(listener) == 0);
    if (mode != 0) {
      assert(send(peer, "WORLD", 5u, 0) == 5);
      h2_pal_net_socket_t out = -1;
      assert((mode == 1 ? esp_net_tls_wrap_bounded : esp_net_tls_wrap_managed)(
                 NULL, fd, &test_config, 1000u, &out) == H2_PAL_OK);
      test_check_hello(peer);
      test_wait_jobs();
    }
    assert(esp_net_tcp_send_timeout_managed(NULL, fd, (const uint8_t*)"DATA",
                                            4u, 100u) == 4);
    assert(pthread_mutex_lock(&test_lock) == 0);
    test_expect_normal_fd = fd;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    esp_net_close(NULL, fd);
    char data[4];
    assert(recv(peer, data, 4u, 0) == 4 && memcmp(data, "DATA", 4u) == 0);
    assert(recv(peer, data, 1u, 0) == 0); /* EOF, never an abortive reset. */
    test_wait_closed(fd);
    assert(close(peer) == 0);
    test_wait_idle();
    assert(pthread_mutex_lock(&test_lock) == 0);
    test_expect_normal_fd = -1;
    assert(pthread_mutex_unlock(&test_lock) == 0);
  }
}
static void test_full_cleanup_queue_preserves_network(void) {
  int first_fd = -1;
  for (size_t i = 0u; i < H2_ESP_NET_TLS_SOCKET_MAX; ++i) {
    int pair[2];
    test_pair(pair, 1);
    h2_pal_net_socket_t out = -1;
    assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 1000u, &out) ==
           H2_PAL_OK);
    test_check_hello(pair[1]);
    test_wait_jobs();
    if (i == 0u) {
      first_fd = out;
      test_gate_begin(6);
    }
    esp_net_close(NULL, out);
    test_wait_closed(out);
    if (i == 0u) {
      test_gate_wait();
      assert(pthread_mutex_lock(&test_lock) == 0);
      test_gate_stage = 0;
      assert(pthread_mutex_unlock(&test_lock) == 0);
    }
    assert(close(pair[1]) == 0);
  }
  /* One destructor is stuck, the other seven slots are queued. No admission
   * can block the close engine, and none may become FREE before destruction. */
  uint64_t until = esp_net_now_ms() + 1000u;
  for (;;) {
    size_t pending = 0u;
    portENTER_CRITICAL(&s_esp_net_mux);
    for (size_t i = 0u; i < H2_ESP_NET_TLS_SOCKET_MAX; ++i)
      pending += s_esp_tls_cleanup[i].pending != 0;
    portEXIT_CRITICAL(&s_esp_net_mux);
    if (pending == H2_ESP_NET_TLS_SOCKET_MAX - 1u) break;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  assert(xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY));
  for (size_t i = 0u; i < H2_ESP_NET_TLS_SOCKET_MAX; ++i)
    assert(s_esp_tls_sockets[i].state == ESP_NET_TLS_SOCKET_CLOSING &&
           s_esp_tls_sockets[i].fd == -1);
  assert(xSemaphoreGive(s_esp_tls_sync->registry_mutex));
  int rejected[2];
  test_pair(rejected, 1);
  h2_pal_net_socket_t out = -1;
  assert(esp_net_tls_wrap_bounded(NULL, rejected[0], &test_config, 100u,
                                  &out) == H2_PAL_ERR_NO_SPACE);
  esp_net_close(NULL, rejected[0]);
  assert(close(rejected[1]) == 0);
  int fresh[2];
  test_reuse_pair(fresh, first_fd);
  assert(esp_net_tcp_send_managed(NULL, fresh[0], (const uint8_t*)"Q", 1u) ==
         1);
  char byte;
  assert(recv(fresh[1], &byte, 1u, 0) == 1 && byte == 'Q');
  test_gate_end();
  until = esp_net_now_ms() + 3000u;
  for (;;) {
    assert(pthread_mutex_lock(&test_lock) == 0);
    int contexts = test_contexts;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    if (contexts == 0) break;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  assert(esp_net_tcp_send_managed(NULL, fresh[0], (const uint8_t*)"Z", 1u) ==
         1);
  assert(recv(fresh[1], &byte, 1u, 0) == 1 && byte == 'Z');
  esp_net_close(NULL, fresh[0]);
  assert(close(fresh[1]) == 0);
  test_wait_idle();
}

int main(int argc, char** argv) {
  if (argc == 2) {
    signal(SIGPIPE, SIG_IGN);
    if (strcmp(argv[1], "legacy-retry") == 0 ||
        strcmp(argv[1], "legacy-sync-retry") == 0) {
      test_retained_legacy_api_retry(strcmp(argv[1], "legacy-sync-retry") == 0);
    } else if (strcmp(argv[1], "cancel-no-reaper") == 0) {
      test_fail_task = 1;
      esp_net_tls_init();
      test_cancel_without_reaper();
    } else {
      esp_net_tls_init();
      if (strcmp(argv[1], "completed-io") == 0)
        test_completed_io_survives_close();
      else if (strcmp(argv[1], "idle-close") == 0)
        test_idle_close_reopens_immediately();
      else if (strcmp(argv[1], "destructor-isolation") == 0)
        test_unrelated_close_during_tls_destructor();
      else if (strcmp(argv[1], "native-close-profiles") == 0) {
        test_failed_bounded_close_has_no_fin_wait();
        test_graceful_close_profiles();
      } else if (strcmp(argv[1], "cleanup-queue") == 0)
        test_full_cleanup_queue_preserves_network();
      else
        abort();
    }
    return 0;
  }

  (void)s_esp_resolver_count;
  signal(SIGPIPE, SIG_IGN);
  test_initialization_failure_and_plain_network();
  test_no_caller_sdk_startup();
  test_worker_allocation_budget();
  test_large_ca();
  test_close_during_copy();
  for (int stage = 1; stage <= 5; ++stage)
    test_close_live_wrap_and_reuse(stage);
  test_close_while_bio_is_active();
  test_capacity_and_network_timeout();
  test_parse_failure_cleanup();
  test_registry_deadlines();
  test_legacy_compatibility_and_close();
  test_deferred_destructor_and_reuse();
  test_late_accepted_cleanup();
  test_public_socket_operations();
  test_completed_io_survives_close();
  test_idle_close_reopens_immediately();
  test_unrelated_close_during_tls_destructor();
  test_failed_bounded_close_has_no_fin_wait();
  test_graceful_close_profiles();
  test_full_cleanup_queue_preserves_network();
  assert(test_task_count == 4);
  puts(
      "ESP production TLS deadlines, initialized resource pool, large CA, close/pins/generations, concurrent wraps and all socket entries PASS");
  return 0;
}
