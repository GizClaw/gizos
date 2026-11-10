/* Compiled with the production Net socket operations and TLS implementation. */
static void test_cancel_without_reaper(void);
static void test_prepare_failure_without_reaper(int failures);
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
  h2_pal_net_addr_t address;
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
  assert(esp_net_socket_register(pair[0], SOCK_STREAM,
                                 ESP_NET_SOCKET_CONNECTION) == H2_PAL_OK);
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
  assert(esp_net_socket_register(pair[0], SOCK_STREAM,
                                 ESP_NET_SOCKET_CONNECTION) == H2_PAL_OK);
  assert(send(pair[1], "WORLD", 5u, 0) == 5);
}
static void test_wait_jobs(void) {
  uint64_t until = esp_net_now_ms() + 3000u;
  for (;;) {
    int busy = 0;
    portENTER_CRITICAL(&s_esp_net_mux);
    for (size_t i = 0; i < H2_ESP_NET_TLS_WORKER_MAX; ++i)
      busy |=
          s_esp_tls_workers[i].job != NULL || s_esp_tls_workers[i].cleanup_busy;
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
  test_fail_task =
      -1; /* Keep the missing static cancellation actor unavailable. */
  test_cancel_without_reaper();
  test_prepare_failure_without_reaper(1);
  test_prepare_failure_without_reaper(3);
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
  test_fail_task =
      -1; /* The unavailable static cancellation actor is retried. */
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
  test_fail_task = 0;
  test_pair(pair, 1);
  /* Reuse the retained API; ordinary TLS must retry initialization itself. */
  assert(h2_pal_net_tls_wrap(&legacy_api, pair[0], &test_config, 1000u, &out) ==
         H2_PAL_OK);
  test_check_hello(pair[1]);
  esp_net_close(NULL, out);
  assert(close(pair[1]) == 0);
  assert(s_esp_tls_initialized && s_esp_tls_pool_ready && test_task_count == 4);
  test_sdk_deferred_drain();
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

static void test_cancel_diagnostic(const char* phase, int fd, int mode,
                                   int result, int error, size_t received,
                                   size_t filled) {
  fprintf(
      stderr,
      "ESP_CANCEL phase=%s mode=%d fd=%d result=%d errno=%d received=%zu filled=%zu flags=%d\n",
      phase, mode, fd, result, error, received, filled, fcntl(fd, F_GETFL, 0));
  struct timeval receive_timeout = {0};
  socklen_t size = sizeof(receive_timeout);
  int option_result =
      getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, &size);
  fprintf(stderr, "ESP_CANCEL SO_RCVTIMEO result=%d sec=%ld usec=%d\n",
          option_result, (long)receive_timeout.tv_sec,
          (int)receive_timeout.tv_usec);
  portENTER_CRITICAL(&s_esp_net_mux);
  fprintf(stderr, "ESP_CANCEL primary_busy=%d native_ready=%d\n",
          s_esp_tls_cleanup_busy, s_esp_net_native_ready);
  for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
    esp_net_socket_record_t* record = &s_esp_net_records[i];
    if (record->used && record->fd == fd)
      fprintf(
          stderr,
          "ESP_CANCEL closed=%d refs=%zu reaping=%d shutdown=%d wrapping=%d role=%d retry=%llu\n",
          record->closed, record->references, record->reaping,
          record->shutdown_started, record->wrapping, record->role,
          (unsigned long long)record->retry_at_ms);
  }
  for (size_t i = 0; i < H2_ESP_NET_TLS_WORKER_MAX; ++i) {
    esp_net_tls_worker_t* worker = &s_esp_tls_workers[i];
    fprintf(
        stderr,
        "ESP_CANCEL worker=%zu cleanup_busy=%d job=%d request=%d decision=%d\n",
        i, worker->cleanup_busy, worker->job != NULL,
        worker->job ? (int)worker->job->request : -1,
        worker->job ? (int)worker->job->decision : -1);
  }
  portEXIT_CRITICAL(&s_esp_net_mux);
  assert(pthread_mutex_lock(&test_lock) == 0);
  fprintf(
      stderr,
      "ESP_CANCEL select fd=%d write=%d timeout_us=%lld result=%d start=%lld end=%lld poll=%d\n",
      test_probe_select_fd, test_probe_select_write,
      (long long)test_probe_select_timeout_us, test_probe_select_result,
      (long long)test_probe_select_start, (long long)test_probe_select_end,
      test_probe_select_poll);
  fprintf(
      stderr,
      "ESP_CANCEL send fd=%d flags=%d result=%d start=%lld end=%lld recv fd=%d flags=%d result=%d start=%lld end=%lld write_wait=%d gate=%d entered=%d released=%d\n",
      test_probe_send_fd, test_probe_send_flags, test_probe_send_result,
      (long long)test_probe_send_start, (long long)test_probe_send_end,
      test_probe_recv_fd, test_probe_recv_flags, test_probe_recv_result,
      (long long)test_probe_recv_start, (long long)test_probe_recv_end,
      test_write_wait_entered, test_gate_stage, test_gate_entered,
      test_gate_release);
  fprintf(
      stderr,
      "ESP_CANCEL holds recv_fd=%d recv_held=%d recv_release=%d cancel_hold=%d cancel_entered=%d cancel_release=%d primary_hold=%d primary_entered=%d primary_release=%d\n",
      test_hold_recv_fd, test_recv_held, test_recv_release,
      test_hold_cancel_claim, test_cancel_claim_entered,
      test_cancel_claim_release, test_hold_cleanup_claim,
      test_cleanup_claim_entered, test_cleanup_claim_release);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  fflush(stderr);
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
    int done = test_call_done(&call, 150u);
    if (!done)
      test_cancel_diagnostic("no-reaper-deadline", fd, accept_call, 0, errno,
                             0u, 0u);
    assert(done);
    assert(pthread_join(thread, NULL) == 0 && call.result == H2_PAL_ERR_CLOSED);
    test_wait_closed(fd);
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
  h2_pal_net_socket_t fd = -1, out = 123;
  assert(esp_net_tcp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, &fd) ==
         H2_PAL_OK);
  /* Retained ordinary API retries initialization before rejecting this honest
   * not-yet-connected handle. No getter or explicit initialization retry. */
  assert(h2_pal_net_tls_wrap(&api, fd, &test_config, 1000u, &out) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(s_esp_tls_initialized && s_esp_tls_pool_ready && out == -1);
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  struct sockaddr_in address = {.sin_family = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  socklen_t length = sizeof(address);
  assert(bind(listener, (struct sockaddr*)&address, length) == 0 &&
         listen(listener, 1) == 0);
  assert(getsockname(listener, (struct sockaddr*)&address, &length) == 0);
  h2_pal_net_addr_t remote = {.family = H2_PAL_NET_FAMILY_IPV4,
                              .port = ntohs(address.sin_port),
                              .ip = {127, 0, 0, 1}};
  assert(esp_net_tcp_connect_managed(NULL, fd, &remote, 1000u) == H2_PAL_OK);
  int peer = accept(listener, NULL, NULL);
  assert(peer >= 0 && close(listener) == 0);
  assert(send(peer, "WORLD", 5u, 0) == 5);
  assert(h2_pal_net_tls_wrap(&api, fd, &test_config, 1000u, &out) == H2_PAL_OK);
  assert(out == fd);
  test_check_hello(peer);
  esp_net_close(NULL, out);
  assert(close(peer) == 0);
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
static void test_queue_closed_before_primary_claim(const int* sockets,
                                                   size_t count);
static void test_full_cleanup_queue_preserves_network(void) {
  int sockets[H2_ESP_NET_TLS_SOCKET_MAX], peers[H2_ESP_NET_TLS_SOCKET_MAX];
  for (size_t i = 0; i < H2_ESP_NET_TLS_SOCKET_MAX; ++i) {
    int pair[2];
    test_pair(pair, 1);
    h2_pal_net_socket_t out = -1;
    assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 1000u, &out) ==
           H2_PAL_OK);
    test_check_hello(pair[1]);
    test_wait_jobs();
    sockets[i] = out;
    peers[i] = pair[1];
  }
  test_gate_begin(6);
  /* Publish the full closed-record backlog before allowing a blocking actor
   * to claim it. Native closes precede destruction in each blocking lane. */
  test_queue_closed_before_primary_claim(sockets, H2_ESP_NET_TLS_SOCKET_MAX);
  for (size_t i = 0; i < H2_ESP_NET_TLS_SOCKET_MAX; ++i) {
    test_wait_closed(sockets[i]);
    assert(close(peers[i]) == 0);
  }
  /* Three independent destructor calls are now stalled: the primary and two
   * helpers. The remaining five slots remain bounded pending work. */
  uint64_t until = esp_net_now_ms() + 1000u;
  for (;;) {
    size_t pending = 0u;
    portENTER_CRITICAL(&s_esp_net_mux);
    for (size_t i = 0; i < H2_ESP_NET_TLS_SOCKET_MAX; ++i)
      pending += s_esp_tls_cleanup[i].pending != 0;
    portEXIT_CRITICAL(&s_esp_net_mux);
    assert(pthread_mutex_lock(&test_lock) == 0);
    int active = test_gate_entered;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    if (active == 3 && pending == H2_ESP_NET_TLS_SOCKET_MAX - 3u) break;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  assert(xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY));
  for (size_t i = 0; i < H2_ESP_NET_TLS_SOCKET_MAX; ++i)
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
  test_reuse_pair(fresh, sockets[0]);
  assert(esp_net_tcp_send_managed(NULL, fresh[0], (const uint8_t*)"Q", 1u) ==
         1);
  char byte;
  assert(recv(fresh[1], &byte, 1u, 0) == 1 && byte == 'Q');
  test_gate_end();
  test_wait_jobs();
  assert(esp_net_tcp_send_managed(NULL, fresh[0], (const uint8_t*)"Z", 1u) ==
         1);
  assert(recv(fresh[1], &byte, 1u, 0) == 1 && byte == 'Z');
  esp_net_close(NULL, fresh[0]);
  assert(close(fresh[1]) == 0);
  test_wait_idle();
}

static int test_fd_closed_within(int fd, uint32_t budget_ms) {
  uint64_t until = esp_net_now_ms() + budget_ms;
  while (fcntl(fd, F_GETFD) >= 0) {
    if (esp_net_now_ms() >= until) return 0;
    test_pause_ms(1u);
  }
  return errno == EBADF;
}
static void test_graceful_close_does_not_block_caller(void) {
  int pair[2];
  test_pair(pair, 0);
  assert(esp_net_tcp_send_timeout_managed(NULL, pair[0], (const uint8_t*)"DATA",
                                          4u, 100u) == 4);
  test_gate_begin(15);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_simulate_fin_wait = 1;
  test_expect_normal_fd = pair[0];
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_call_t call = {.fd = pair[0]};
  pthread_t closer;
  assert(pthread_create(&closer, NULL, test_call_close, &call) == 0);
  assert(test_call_done(&call, 150u));
  assert(pthread_join(closer, NULL) == 0);
  test_gate_wait();
  h2_pal_net_socket_t listener = -1, rebound = -1;
  h2_pal_net_addr_t address, again;
  assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                    &listener, &address) == H2_PAL_OK);
  esp_net_close(NULL, listener);
  assert(fcntl(listener, F_GETFD) < 0 && errno == EBADF);
  assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, address.port,
                                    NULL, &rebound, &again) == H2_PAL_OK);
  esp_net_close(NULL, rebound);
  test_gate_end();
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_simulate_fin_wait = 0;
  test_expect_normal_fd = -1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  char data[4];
  assert(recv(pair[1], data, 4u, 0) == 4 && memcmp(data, "DATA", 4u) == 0);
  assert(recv(pair[1], data, 1u, 0) == 0);
  test_wait_closed(pair[0]);
  assert(close(pair[1]) == 0);
  test_wait_idle();
}
static int test_sem_asleep(SemaphoreHandle_t semaphore) {
  assert(pthread_mutex_lock(&semaphore->mutex) == 0);
  int asleep = semaphore->available == 0 && semaphore->waiters > 0;
  assert(pthread_mutex_unlock(&semaphore->mutex) == 0);
  return asleep;
}
static void test_wait_executors_asleep(void) {
  uint64_t idle_deadline = esp_net_now_ms() + 1000u;
  for (;;) {
    int asleep = test_sem_asleep(s_esp_tls_cleanup_wake) &&
                 test_sem_asleep(s_esp_net_reaper_wake);
    for (size_t i = 0; i < H2_ESP_NET_TLS_WORKER_MAX; ++i)
      asleep &= test_sem_asleep(s_esp_tls_workers[i].wake);
    if (asleep) break;
    assert(esp_net_now_ms() < idle_deadline);
    test_pause_ms(1u);
  }
}
static void test_queue_closed_before_primary_claim(const int* sockets,
                                                   size_t count) {
  test_wait_executors_asleep();
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_hold_cleanup_claim = 1;
  test_cleanup_claim_entered = test_cleanup_claim_release = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  for (size_t i = 0; i < count; ++i) esp_net_close(NULL, sockets[i]);
  uint64_t until = esp_net_now_ms() + 1000u;
  for (;;) {
    int asleep = test_sem_asleep(s_esp_net_reaper_wake);
    for (size_t i = 0; i < H2_ESP_NET_TLS_WORKER_MAX; ++i)
      asleep &= test_sem_asleep(s_esp_tls_workers[i].wake);
    assert(pthread_mutex_lock(&test_lock) == 0);
    int held = test_cleanup_claim_entered;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    if (held && asleep) break;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_hold_cleanup_claim = 0;
  test_cleanup_claim_release = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
}
static void test_one_stalled_destructor_does_not_exhaust_slots(void) {
  int sockets[H2_ESP_NET_TLS_SOCKET_MAX], peers[H2_ESP_NET_TLS_SOCKET_MAX];
  for (size_t i = 0; i < H2_ESP_NET_TLS_SOCKET_MAX; ++i) {
    int pair[2];
    test_pair(pair, 1);
    h2_pal_net_socket_t out = -1;
    assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 1000u, &out) ==
           H2_PAL_OK);
    test_check_hello(pair[1]);
    test_wait_jobs();
    sockets[i] = out;
    peers[i] = pair[1];
  }
  test_gate_begin(6);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_primary_only = 1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  /* Helpers first consume their old wakes while the primary is paused and
   * idle. Releasing only the primary must wake them at its first work claim. */
  test_queue_closed_before_primary_claim(sockets, H2_ESP_NET_TLS_SOCKET_MAX);
  test_gate_wait();
  uint64_t until = esp_net_now_ms() + 250u;
  for (;;) {
    assert(pthread_mutex_lock(&test_lock) == 0);
    int contexts = test_contexts;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    if (contexts == 1) break;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  for (size_t i = 0; i < H2_ESP_NET_TLS_SOCKET_MAX; ++i) {
    test_wait_closed(sockets[i]);
    assert(close(peers[i]) == 0);
  }
  test_wait_jobs(); /* Readers only: no new request/wake before reclamation. */
  int fresh[2];
  test_pair(fresh, 1);
  h2_pal_net_socket_t out = -1;
  assert(esp_net_tls_wrap_bounded(NULL, fresh[0], &test_config, 1000u, &out) ==
         H2_PAL_OK);
  test_check_hello(fresh[1]);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_hold_cleanup_claim = 0;
  test_gate_primary_only = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_gate_end();
  test_wait_jobs();
  assert(esp_net_tcp_send_timeout_managed(NULL, out, (const uint8_t*)"LIVE", 4u,
                                          100u) == 4);
  char data[4];
  assert(recv(fresh[1], data, 4u, 0) == 4 && memcmp(data, "LIVE", 4u) == 0);
  esp_net_close(NULL, out);
  assert(close(fresh[1]) == 0);
  test_wait_idle();
}
static void test_prepare_failure_without_reaper(int failures) {
  int pair[2];
  test_pair(pair, 0);
  assert(esp_net_tcp_send_timeout_managed(NULL, pair[0], (const uint8_t*)"DATA",
                                          4u, 100u) == 4);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_fail_normal_linger = failures;
  test_expect_normal_fd = pair[0];
  test_normal_linger_attempts = 0;
  test_linger_first_failure_ms = test_linger_last_failure_ms = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_call_t call = {.fd = pair[0]};
  pthread_t reader;
  if (failures > 1) {
    assert(pthread_create(&reader, NULL, test_cancellable_receive, &call) == 0);
    test_wait_pinned(pair[0]);
  }
  esp_net_close(NULL, pair[0]);
  assert(test_fd_closed_within(pair[0], 150u));
  if (failures > 1)
    assert(pthread_join(reader, NULL) == 0 && call.result == H2_PAL_ERR_CLOSED);
  assert(pthread_mutex_lock(&test_lock) == 0);
  assert(test_fail_normal_linger == 0 &&
         test_normal_linger_attempts == failures + 1);
  if (failures > 1)
    assert(test_linger_last_failure_ms - test_linger_first_failure_ms >=
           (uint64_t)(failures - 1));
  test_expect_normal_fd = -1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  char data[4];
  assert(recv(pair[1], data, 4u, 0) == 4 && memcmp(data, "DATA", 4u) == 0);
  assert(recv(pair[1], data, 1u, 0) == 0);
  assert(close(pair[1]) == 0);
  test_wait_idle();
}

static void test_all_executors_unavailable(void) {
  /* Keep injected startup failures independent of SDK callback scheduling. */
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_daemon_hold = 1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_fail_task = -1;
  esp_net_tls_init();
  assert(!esp_net_socket_has_executor() && !s_esp_tls_pool_ready);
  h2_pal_net_socket_t fd = -1, listener = -1;
  h2_pal_net_addr_t address;
  assert(esp_net_tcp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, &fd) ==
         H2_PAL_OK);
  assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                    &listener, &address) == H2_PAL_OK);
  address.ip[0] = 127;
  address.ip[1] = address.ip[2] = 0;
  address.ip[3] = 1;
  int connects = test_native_connect_calls, accepts = test_native_accept_calls;
  assert(esp_net_tcp_connect_managed(NULL, fd, &address, 100u) ==
         H2_PAL_ERR_UNAVAILABLE);
  h2_pal_net_socket_t incoming = -1;
  assert(esp_net_tcp_accept_managed(NULL, listener, &incoming, NULL, 0u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(test_native_connect_calls == connects &&
         test_native_accept_calls == accepts);
  esp_net_close(NULL, fd);
  esp_net_close(NULL, listener);
  assert(fcntl(fd, F_GETFD) < 0 && errno == EBADF);
  assert(fcntl(listener, F_GETFD) < 0 && errno == EBADF);
  h2_pal_net_socket_t rebound = -1;
  h2_pal_net_addr_t again;
  assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, address.port,
                                    NULL, &rebound, &again) == H2_PAL_OK);
  esp_net_close(NULL, rebound);
  h2_pal_net_socket_t udp1 = -1, udp2 = -1;
  h2_pal_net_addr_t to;
  assert(esp_net_udp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, &udp1,
                                  &again) == H2_PAL_OK);
  assert(esp_net_udp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, &udp2,
                                  &to) == H2_PAL_OK);
  to.ip[0] = 127;
  to.ip[1] = to.ip[2] = 0;
  to.ip[3] = 1;
  assert(esp_net_udp_sendto_managed(NULL, udp1, &to, (const uint8_t*)"OK",
                                    2u) == 2);
  uint8_t data[2];
  assert(esp_net_udp_recvfrom_managed(NULL, udp2, NULL, data, 2u, 100u) == 2 &&
         memcmp(data, "OK", 2u) == 0);
  esp_net_close(NULL, udp1);
  esp_net_close(NULL, udp2);
  assert(fcntl(udp1, F_GETFD) < 0 && errno == EBADF);
  assert(fcntl(udp2, F_GETFD) < 0 && errno == EBADF);
  test_fail_task = 0;
  test_wait_idle();
}

static void test_unpublished_accept_cleanup(int adopt_failure) {
  h2_pal_net_socket_t listener = -1;
  h2_pal_net_addr_t address;
  assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                    &listener, &address) == H2_PAL_OK);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_hold_accept = 1;
  test_accept_entered = test_accept_release = 0;
  test_last_accepted_fd = -1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_call_t call = {.fd = listener, .legacy = 1, .out = 123};
  pthread_t receiver;
  assert(pthread_create(&receiver, NULL, test_cancellable_receive, &call) == 0);
  int peer = socket(AF_INET, SOCK_STREAM, 0);
  assert(peer >= 0);
  struct sockaddr_in remote = {.sin_family = AF_INET,
                               .sin_port = htons(address.port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(connect(peer, (struct sockaddr*)&remote, sizeof(remote)) == 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  while (!test_accept_entered)
    assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
  int accepted = test_last_accepted_fd;
  assert(accepted >= 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  if (!adopt_failure) esp_net_close(NULL, listener);
  test_gate_begin(15);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_simulate_fin_wait = 1;
  test_expect_normal_fd = accepted;
  if (adopt_failure == 1) test_fail_abortive_linger = 1;
  if (adopt_failure >= 2)
    test_fail_accepted_fcntl_command = adopt_failure == 2 ? F_GETFL : F_SETFL;
  test_accept_release = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(test_call_done(&call, 150u));
  assert(pthread_join(receiver, NULL) == 0);
  int expected = adopt_failure == 0   ? H2_PAL_ERR_CLOSED
                 : adopt_failure == 1 ? H2_PAL_ERR_UNAVAILABLE
                                      : H2_PAL_ERR_IO;
  assert(call.result == expected && call.out == -1);
  test_gate_wait();
  assert(fcntl(accepted, F_GETFD) >=
         0); /* Only the background owner is in FIN wait. */
  int unrelated[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, unrelated) == 0);
  assert(unrelated[0] != accepted && unrelated[1] != accepted);
  if (adopt_failure) {
    h2_pal_net_socket_t again = -1;
    int peer2 = socket(AF_INET, SOCK_STREAM, 0);
    assert(peer2 >= 0);
    assert(connect(peer2, (struct sockaddr*)&remote, sizeof(remote)) == 0);
    assert(esp_net_tcp_accept_managed(NULL, listener, &again, NULL, 1000u) ==
           H2_PAL_OK);
    assert(esp_net_tcp_send_timeout_managed(NULL, again, (const uint8_t*)"NEXT",
                                            4u, 100u) == 4);
    char bytes[4];
    assert(recv(peer2, bytes, 4u, 0) == 4 && memcmp(bytes, "NEXT", 4u) == 0);
    esp_net_close(NULL, again);
    /* Keep the peer alive until the stalled close owner has resumed. */
    test_gate_end();
    assert(pthread_mutex_lock(&test_lock) == 0);
    test_simulate_fin_wait = 0;
    test_expect_normal_fd = -1;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    assert(recv(peer2, bytes, 1u, 0) == 0);
    assert(close(peer2) == 0);
    esp_net_close(NULL, listener);
  } else {
    test_gate_end();
    assert(pthread_mutex_lock(&test_lock) == 0);
    test_simulate_fin_wait = 0;
    test_expect_normal_fd = -1;
    assert(pthread_mutex_unlock(&test_lock) == 0);
  }
  test_wait_closed(accepted);
  char byte;
  assert(recv(peer, &byte, 1u, 0) == 0);
  assert(close(peer) == 0);
  assert(close(unrelated[0]) == 0 && close(unrelated[1]) == 0);
  int fresh[2];
  test_reuse_pair(fresh, accepted);
  assert(esp_net_tcp_send_timeout_managed(
             NULL, fresh[0], (const uint8_t*)"LIVE", 4u, 100u) == 4);
  char data[4];
  assert(recv(fresh[1], data, 4u, 0) == 4 && memcmp(data, "LIVE", 4u) == 0);
  esp_net_close(NULL, fresh[0]);
  assert(close(fresh[1]) == 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_hold_accept = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_wait_idle();
}

static void test_accept_capacity_reserved_before_native(void) {
  h2_pal_net_socket_t listener = -1, fresh[CONFIG_LWIP_MAX_SOCKETS - 1u];
  h2_pal_net_addr_t address;
  assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                    &listener, &address) == H2_PAL_OK);
  for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS - 1u; ++i)
    assert(esp_net_tcp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, &fresh[i]) ==
           H2_PAL_OK);
  int peer = socket(AF_INET, SOCK_STREAM, 0);
  assert(peer >= 0);
  struct sockaddr_in remote = {.sin_family = AF_INET,
                               .sin_port = htons(address.port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(connect(peer, (struct sockaddr*)&remote, sizeof(remote)) == 0);
  int calls = test_native_accept_calls;
  h2_pal_net_socket_t out = 123;
  assert(esp_net_tcp_accept_managed(NULL, listener, &out, NULL, 0u) ==
         H2_PAL_ERR_NO_SPACE);
  assert(out == -1 && test_native_accept_calls == calls);
  for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS - 1u; ++i)
    esp_net_close(NULL, fresh[i]);
  assert(esp_net_tcp_accept_managed(NULL, listener, &out, NULL, 1000u) ==
         H2_PAL_OK);
  assert(esp_net_tcp_send_timeout_managed(NULL, out, (const uint8_t*)"OK", 2u,
                                          100u) == 2);
  char data[2];
  assert(recv(peer, data, 2u, 0) == 2 && memcmp(data, "OK", 2u) == 0);
  esp_net_close(NULL, out);
  esp_net_close(NULL, listener);
  assert(recv(peer, data, 1u, 0) == 0);
  assert(close(peer) == 0);
  test_wait_idle();
}

static void test_accept_empty_poll_and_negative_ticket(void) {
  h2_pal_net_socket_t listener = -1;
  h2_pal_net_addr_t address;
  assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                    &listener, &address) == H2_PAL_OK);
  esp_net_socket_lease_t ticket;
  assert(esp_net_socket_reserve_accept(&ticket) == H2_PAL_OK);
  esp_net_close(NULL, -1);
  portENTER_CRITICAL(&s_esp_net_mux);
  esp_net_socket_record_t record = s_esp_net_records[ticket.index];
  assert(record.used && !record.closed && record.unpublished &&
         record.fd == -1 && record.references == 1u &&
         record.generation == ticket.generation);
  s_esp_net_records[ticket.index].closed = 1;
  portEXIT_CRITICAL(&s_esp_net_mux);
  esp_net_socket_reap_t work;
  uint64_t now = esp_net_now_ms();
  portENTER_CRITICAL(&s_esp_net_mux);
  assert(!esp_net_socket_reap_take_locked(&work, now)); /* No negative fd. */
  portEXIT_CRITICAL(&s_esp_net_mux);
  esp_net_socket_cancel_accept(&ticket);
  h2_pal_net_socket_t out = 123;
  for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS + 2u; ++i) {
    assert(esp_net_tcp_accept_managed(NULL, listener, &out, NULL, 0u) ==
               H2_PAL_ERR_TIMEOUT &&
           out == -1);
  }
  int peer = socket(AF_INET, SOCK_STREAM, 0);
  assert(peer >= 0);
  struct sockaddr_in remote = {.sin_family = AF_INET,
                               .sin_port = htons(address.port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(connect(peer, (struct sockaddr*)&remote, sizeof(remote)) == 0);
  /* connect completion need not make a zero-timeout listener poll ready in
   * the same scheduling instant. Establish readiness before injecting EAGAIN.
   */
  assert(h2_esp_net_wait_fd(listener, 0, 1000u) == H2_PAL_OK);
  for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS + 2u; ++i) {
    assert(pthread_mutex_lock(&test_lock) == 0);
    test_accept_would_block = 1;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    assert(esp_net_tcp_accept_managed(NULL, listener, &out, NULL, 0u) ==
               H2_PAL_ERR_WOULD_BLOCK &&
           out == -1);
  }
  assert(esp_net_tcp_accept_managed(NULL, listener, &out, NULL, 1000u) ==
         H2_PAL_OK);
  assert(esp_net_tcp_send_timeout_managed(NULL, out, (const uint8_t*)"OK", 2u,
                                          100u) == 2);
  char data[2];
  assert(recv(peer, data, 2u, 0) == 2 && memcmp(data, "OK", 2u) == 0);
  esp_net_close(NULL, out);
  esp_net_close(NULL, listener);
  assert(recv(peer, data, 1u, 0) == 0);
  assert(close(peer) == 0);
  test_wait_idle();
}

static void test_receive_cancel_during_other_graceful_close(void) {
  int first[2];
  test_pair(first, 0);
  assert(esp_net_tcp_send_timeout_managed(
             NULL, first[0], (const uint8_t*)"DATA", 4u, 100u) == 4);
  test_gate_begin(15);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_simulate_fin_wait = 1;
  test_fin_wait_fd = first[0];
  test_expect_normal_fd = first[0];
  assert(pthread_mutex_unlock(&test_lock) == 0);
  esp_net_close(NULL, first[0]);
  test_gate_wait();
  int second[2];
  test_pair(second, 0);
  test_call_t call = {.fd = second[0]};
  pthread_t receiver;
  assert(pthread_create(&receiver, NULL, test_cancellable_receive, &call) == 0);
  test_wait_pinned(second[0]);
  esp_net_close(NULL, second[0]);
  assert(test_call_done(&call, 150u));
  assert(pthread_join(receiver, NULL) == 0 && call.result == H2_PAL_ERR_CLOSED);
  assert(test_fd_closed_within(second[0], 150u));
  char byte;
  assert(recv(second[1], &byte, 1u, 0) == 0);
  assert(close(second[1]) == 0);
  test_gate_end();
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_simulate_fin_wait = 0;
  test_fin_wait_fd = -1;
  test_expect_normal_fd = -1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  char data[4];
  assert(recv(first[1], data, 4u, 0) == 4 && memcmp(data, "DATA", 4u) == 0);
  assert(recv(first[1], data, 1u, 0) == 0);
  assert(close(first[1]) == 0);
  test_wait_idle();
}
static void test_queued_cancel_on_native_busy_transition(void) {
  int first[2], second[2];
  test_pair(first, 0);
  test_pair(second, 0);
  assert(esp_net_tcp_send_timeout_managed(
             NULL, first[0], (const uint8_t*)"DATA", 4u, 100u) == 4);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_hold_recv_fd = second[0];
  test_recv_held = test_recv_release = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_call_t call = {.fd = second[0]};
  pthread_t receiver;
  assert(pthread_create(&receiver, NULL, test_cancellable_receive, &call) == 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  while (!test_recv_held)
    assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_wait_executors_asleep();
  test_gate_begin(15);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_hold_cleanup_claim = test_hold_cancel_claim = 1;
  test_cleanup_claim_entered = test_cleanup_claim_release = 0;
  test_cancel_claim_entered = test_cancel_claim_release = 0;
  test_simulate_fin_wait = 1;
  test_fin_wait_fd = first[0];
  test_expect_normal_fd = first[0];
  assert(pthread_mutex_unlock(&test_lock) == 0);
  esp_net_close(NULL, first[0]);
  esp_net_close(NULL, second[0]);
  uint64_t until = esp_net_now_ms() + 1000u;
  for (;;) {
    int asleep = 1;
    for (size_t i = 0; i < H2_ESP_NET_TLS_WORKER_MAX; ++i)
      asleep &= test_sem_asleep(s_esp_tls_workers[i].wake);
    assert(pthread_mutex_lock(&test_lock) == 0);
    int held = test_cleanup_claim_entered && test_cancel_claim_entered;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    if (held && asleep) break;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  /* Both closes and the cancellation wake predate native busy. From here only
   * SDK barriers are released: no PAL request or injected semaphore wake. */
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_cleanup_claim_release = 1;
  test_hold_cleanup_claim = 0;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_gate_wait();
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_cancel_claim_release = test_recv_release = 1;
  test_hold_cancel_claim = 0;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  int done = test_call_done(&call, 150u);
  if (!done)
    test_cancel_diagnostic("queued-cancel-deadline", second[0], 3, 0, errno, 0u,
                           0u);
  assert(done);
  assert(pthread_join(receiver, NULL) == 0 && call.result == H2_PAL_ERR_CLOSED);
  assert(test_fd_closed_within(second[0], 150u));
  char data[4];
  assert(recv(second[1], data, 1u, 0) == 0);
  assert(close(second[1]) == 0);
  test_gate_end();
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_simulate_fin_wait = 0;
  test_fin_wait_fd = -1;
  test_expect_normal_fd = -1;
  test_hold_recv_fd = -1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(recv(first[1], data, 4u, 0) == 4 && memcmp(data, "DATA", 4u) == 0);
  assert(recv(first[1], data, 1u, 0) == 0);
  assert(close(first[1]) == 0);
  test_wait_idle();
}
static size_t test_fill_send_buffer(int fd) {
  int size = 4096;
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
  int flags = fcntl(fd, F_GETFL, 0);
  assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
  unsigned char bytes[4096];
  memset(bytes, 'F', sizeof(bytes));
  size_t total = 0;
  for (;;) {
    int sent = send(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
    if (sent < 0) {
      assert(errno == EAGAIN || errno == EWOULDBLOCK);
      return total;
    }
    assert(sent > 0);
    total += (size_t)sent;
  }
}
static void test_wait_write(void) {
  assert(pthread_mutex_lock(&test_lock) == 0);
  struct timespec until = test_after_ms(1000u);
  while (!test_write_wait_entered)
    assert(pthread_cond_timedwait(&test_changed, &test_lock, &until) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
}
static void* test_pending_send(void* context) {
  test_call_t* call = context;
  call->result = esp_net_tcp_send_timeout_managed(
      NULL, call->fd, (const uint8_t*)"LATE", 4u, 3000u);
  assert(pthread_mutex_lock(&test_lock) == 0);
  call->done = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return NULL;
}
static void test_write_cancel_during_other_graceful_close(
    int tls, int bounded_handshake) {
  int pair[2];
  test_pair(pair, tls || bounded_handshake);
  if (tls) {
    h2_pal_net_socket_t out = -1;
    assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 1000u, &out) ==
           H2_PAL_OK);
    test_check_hello(pair[1]);
    test_wait_jobs();
  }
  size_t filled = test_fill_send_buffer(pair[0]);
  assert(filled > 0);
  int first[2];
  test_pair(first, 0);
  test_gate_begin(15);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_simulate_fin_wait = 1;
  test_fin_wait_fd = first[0];
  test_expect_normal_fd = first[0];
  test_write_wait_fd = pair[0];
  test_write_wait_entered = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  esp_net_close(NULL, first[0]);
  test_gate_wait();
  test_call_t call = {
      .fd = pair[0], .timeout_ms = 3000u, .config = &test_config};
  pthread_t sender;
  assert(pthread_create(&sender, NULL,
                        bounded_handshake ? test_call_wrap : test_pending_send,
                        &call) == 0);
  test_wait_write();
  if (tls) {
    assert(xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY));
    int slot = esp_net_tls_find(pair[0]);
    assert(slot >= 0);
    assert(!xSemaphoreTake(s_esp_tls_sync->io_mutexes[slot], 0u));
    assert(xSemaphoreGive(s_esp_tls_sync->registry_mutex));
  }
  portENTER_CRITICAL(&s_esp_net_mux);
  int matched = 0;
  for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
    esp_net_socket_record_t* record = &s_esp_net_records[i];
    if (record->used && record->fd == pair[0]) {
      assert(record->abortive_close == bounded_handshake);
      matched = 1;
      break;
    }
  }
  assert(matched);
  portEXIT_CRITICAL(&s_esp_net_mux);
  esp_net_close(NULL, pair[0]);
  int mode = bounded_handshake ? 2 : tls ? 1 : 0;
  int done = test_call_done(&call, 150u);
  if (!done)
    test_cancel_diagnostic("write-deadline", pair[0], mode, 0, errno, 0u,
                           filled);
  assert(done);
  assert(pthread_join(sender, NULL) == 0);
  assert(call.result == H2_PAL_ERR_CLOSED &&
         (!bounded_handshake || call.out == -1));
  assert(test_fd_closed_within(pair[0], 150u));
  if (tls) {
    assert(test_ssl_write_pending);
    test_ssl_write_pending = 0;
  }
  size_t received = 0;
  unsigned char bytes[4096];
  int n;
  while ((n = recv(pair[1], bytes, sizeof(bytes), 0)) > 0) {
    for (int i = 0; i < n; ++i) assert(bytes[i] == 'F');
    received += (size_t)n;
  }
  int receive_error = errno;
  /* The failed/unpublished handshake deliberately retains abortive cleanup.
   * Linux reports its reset after delivering this fixture's full filler. Plain
   * and successfully published TLS still require every byte and normal EOF. */
  int ended =
      n == 0 || (bounded_handshake && n < 0 && receive_error == ECONNRESET);
  if (!ended || received != filled)
    test_cancel_diagnostic("write-drain", pair[0], mode, n, receive_error,
                           received, filled);
  assert(ended && received == filled);
  assert(close(pair[1]) == 0);
  test_gate_end();
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_simulate_fin_wait = 0;
  test_fin_wait_fd = -1;
  test_expect_normal_fd = -1;
  test_write_wait_fd = -1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(recv(first[1], bytes, 1u, 0) == 0);
  assert(close(first[1]) == 0);
  test_wait_idle();
}
static void test_tls_write_retry_prefix_and_close(void) {
  int pair[2];
  test_pair(pair, 1);
  h2_pal_net_socket_t out = -1;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 1000u, &out) ==
         H2_PAL_OK);
  test_check_hello(pair[1]);
  test_wait_jobs();
  size_t filled = test_fill_send_buffer(pair[0]);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_write_wait_fd = pair[0];
  test_write_wait_entered = 0;
  test_send_prefix_fd = pair[0];
  test_send_prefix_length = 2u;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  int retries = test_ssl_write_retries;
  test_gate_begin(12);
  test_call_t call = {.fd = pair[0]};
  pthread_t sender;
  assert(pthread_create(&sender, NULL, test_pending_send, &call) == 0);
  test_wait_write();
  assert(xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY));
  int slot = esp_net_tls_find(pair[0]);
  assert(slot >= 0);
  assert(!xSemaphoreTake(s_esp_tls_sync->io_mutexes[slot], 0u));
  assert(xSemaphoreGive(s_esp_tls_sync->registry_mutex));
  unsigned char bytes[4096];
  size_t drained = 0u;
  while (drained < filled) {
    size_t size = filled - drained;
    if (size > sizeof(bytes)) size = sizeof(bytes);
    int n = recv(pair[1], bytes, size, 0);
    assert(n > 0);
    for (int i = 0; i < n; ++i) assert(bytes[i] == 'F');
    drained += (size_t)n;
  }
  test_gate_wait(); /* The retry consumed its two-byte prefix before close. */
  esp_net_close(NULL, pair[0]);
  test_gate_end();
  assert(pthread_join(sender, NULL) == 0 && call.result == 2);
  assert(test_ssl_write_retries > retries && !test_ssl_write_pending);
  assert(recv(pair[1], bytes, 2u, 0) == 2 && memcmp(bytes, "LA", 2u) == 0);
  assert(recv(pair[1], bytes, 1u, 0) == 0);
  assert(close(pair[1]) == 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_write_wait_fd = test_send_prefix_fd = -1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_wait_idle();
}
static void test_native_init_budget(void) {
  h2_pal_net_socket_t fd = -1;
  assert(esp_net_tcp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, &fd) ==
         H2_PAL_OK);
  h2_pal_net_addr_t address = {
      .family = H2_PAL_NET_FAMILY_IPV4, .port = 1, .ip = {127, 0, 0, 1}};
  test_static_delay_ms = 4u;
  assert(esp_net_tcp_connect_managed(NULL, fd, &address, 2u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(test_native_connect_calls == 0 && test_task_count <= 1 &&
         test_dynamic_task_calls == 0 && test_caller_allocations == 0);
  esp_net_close(NULL, fd);
}
static void* test_call_native_init(void* context) {
  test_call_t* call = context;
  call->result = esp_net_native_init(0u);
  assert(pthread_mutex_lock(&test_lock) == 0);
  call->done = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return NULL;
}
static void test_native_init_concurrent(void) {
  test_gate_begin(16);
  test_call_t call = {0};
  pthread_t initializer;
  assert(pthread_create(&initializer, NULL, test_call_native_init, &call) == 0);
  test_gate_wait();
  uint64_t started = esp_net_now_ms();
  for (int i = 0; i < 8; ++i) assert(!esp_net_native_init(0u));
  assert(esp_net_now_ms() - started < 100u);
  assert(pthread_mutex_lock(&test_lock) == 0);
  assert(test_static_task_attempts == 1);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_gate_end();
  assert(pthread_join(initializer, NULL) == 0 && call.result);
  assert(test_task_count == 4 && test_static_task_attempts == 4 &&
         test_dynamic_task_calls == 0 && test_caller_allocations == 0);
}
static void test_retained_plain_tcp_recovery(int accept_path,
                                             int tls_heap_failure) {
  const h2_pal_net_vtable_t table = {.tcp_connect = esp_net_tcp_connect_managed,
                                     .tcp_accept = esp_net_tcp_accept_managed};
  const h2_pal_net_api_t retained = {.vtable = &table};
  test_fail_task = tls_heap_failure ? 0 : -1;
  test_alloc_persistent = tls_heap_failure;
  esp_net_tls_init();
  if (tls_heap_failure)
    assert(s_esp_tls_sync == NULL && esp_net_socket_has_executor());
  int allocations = test_caller_allocations,
      dynamic_tasks = test_dynamic_task_calls;
  h2_pal_net_socket_t fd = -1;
  h2_pal_net_addr_t address;
  int peer = -1, external_listener = -1;
  if (accept_path) {
    assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                      &fd, &address) == H2_PAL_OK);
    peer = socket(AF_INET, SOCK_STREAM, 0);
    assert(peer >= 0);
    struct sockaddr_in target = {.sin_family = AF_INET,
                                 .sin_port = htons(address.port),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(connect(peer, (struct sockaddr*)&target, sizeof(target)) == 0);
  } else {
    assert(esp_net_tcp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, &fd) ==
           H2_PAL_OK);
    external_listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(external_listener >= 0);
    struct sockaddr_in local = {.sin_family = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(local);
    assert(bind(external_listener, (struct sockaddr*)&local, length) == 0 &&
           listen(external_listener, 1) == 0);
    assert(getsockname(external_listener, (struct sockaddr*)&local, &length) ==
           0);
    address = (h2_pal_net_addr_t){.family = H2_PAL_NET_FAMILY_IPV4,
                                  .port = ntohs(local.sin_port),
                                  .ip = {127, 0, 0, 1}};
  }
  test_fail_task =
      0; /* Capacity restored; do not fetch API or call legacy TLS. */
  if (accept_path) {
    h2_pal_net_socket_t accepted = -1;
    assert(h2_pal_net_tcp_accept(&retained, fd, &accepted, NULL, 1000u) ==
           H2_PAL_OK);
    esp_net_close(NULL, fd);
    fd = accepted;
  } else {
    assert(h2_pal_net_tcp_connect(&retained, fd, &address, 1000u) == H2_PAL_OK);
    peer = accept(external_listener, NULL, NULL);
    assert(peer >= 0);
    assert(close(external_listener) == 0);
  }
  assert(esp_net_tcp_send_timeout_managed(NULL, fd, (const uint8_t*)"HEAL", 4u,
                                          100u) == 4);
  char data[4];
  assert(recv(peer, data, 4u, 0) == 4 && memcmp(data, "HEAL", 4u) == 0);
  esp_net_close(NULL, fd);
  assert(recv(peer, data, 1u, 0) == 0);
  assert(close(peer) == 0);
  test_wait_closed(fd);
  assert(test_caller_allocations == allocations &&
         test_dynamic_task_calls == dynamic_tasks && dynamic_tasks == 0);
  if (tls_heap_failure)
    assert(s_esp_tls_sync == NULL && test_allocations == 0);
  else
    test_wait_idle();
}

static void* test_ordinary_send(void* context) {
  test_call_t* call = context;
  call->result =
      esp_net_tcp_send_managed(NULL, call->fd, (const uint8_t*)"LATE", 4u);
  assert(pthread_mutex_lock(&test_lock) == 0);
  call->done = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return NULL;
}
static void test_ordinary_send_cancel(void) {
  int pair[2];
  test_pair(pair, 0);
  size_t filled = test_fill_send_buffer(pair[0]);
  int flags = fcntl(pair[0], F_GETFL, 0);
  assert(flags >= 0 && fcntl(pair[0], F_SETFL, flags & ~O_NONBLOCK) == 0);
  test_emulate_dontwait_fd = pair[0];
  test_plain_send_entered = 0;
  test_call_t call = {.fd = pair[0]};
  pthread_t sender;
  assert(pthread_create(&sender, NULL, test_ordinary_send, &call) == 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  struct timespec until = test_after_ms(1000u);
  while (!test_plain_send_entered)
    assert(pthread_cond_timedwait(&test_changed, &test_lock, &until) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  esp_net_close(NULL, pair[0]);
  int done = test_call_done(&call, 150u);
  if (!done)
    test_cancel_diagnostic("ordinary-send-deadline", pair[0], 4, 0, errno, 0u,
                           filled);
  assert(done);
  assert(pthread_join(sender, NULL) == 0 && call.result == H2_PAL_ERR_CLOSED);
  assert(test_fd_closed_within(pair[0], 150u));
  unsigned char bytes[4096];
  size_t received = 0;
  int n;
  while ((n = recv(pair[1], bytes, sizeof(bytes), 0)) > 0) {
    for (int i = 0; i < n; ++i) assert(bytes[i] == 'F');
    received += (size_t)n;
  }
  assert(n == 0 && received == filled);
  assert(close(pair[1]) == 0);
  test_emulate_dontwait_fd = -1;
  test_wait_idle();
}
static void test_retry_after_early_tick(void) {
  int pair[2];
  test_pair(pair, 0);
  assert(esp_net_tcp_send_timeout_managed(NULL, pair[0], (const uint8_t*)"DATA",
                                          4u, 100u) == 4);
  int64_t now = esp_timer_get_time();
  atomic_store(&test_clock_fixed_us, now);
  atomic_store(&test_clock_resume_us, now + 20000);
  atomic_store(&test_early_delay_observed, 0);
  atomic_store(&test_early_delay_return, 1);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_fail_normal_linger = 1;
  test_normal_linger_attempts = 0;
  test_expect_normal_fd = pair[0];
  assert(pthread_mutex_unlock(&test_lock) == 0);
  esp_net_close(NULL, pair[0]);
  /* SDK next tick is early; monotonic time advances later without injecting
   * a new PAL request or queue wake. Only the cleanup retry can make progress.
   */
  assert(test_fd_closed_within(pair[0], 150u));
  assert(atomic_load(&test_early_delay_observed));
  atomic_store(&test_clock_resume_us, 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  assert(test_normal_linger_attempts == 2 && !test_fail_normal_linger);
  test_expect_normal_fd = -1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  char bytes[4];
  assert(recv(pair[1], bytes, 4u, 0) == 4 && memcmp(bytes, "DATA", 4u) == 0);
  assert(recv(pair[1], bytes, 1u, 0) == 0);
  assert(close(pair[1]) == 0);
  test_wait_idle();
}
static void test_zero_timeout_recovery(int accept_path, int some_ready) {
  test_fail_task = some_ready ? 1 : -1;
  esp_net_tls_init();
  test_fail_task = 0;
  h2_pal_net_socket_t fd = -1, out = 123;
  h2_pal_net_addr_t address = {
      .family = H2_PAL_NET_FAMILY_IPV4, .port = 1, .ip = {127, 0, 0, 1}};
  if (accept_path)
    assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                      &fd, &address) == H2_PAL_OK);
  else
    assert(esp_net_tcp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, &fd) ==
           H2_PAL_OK);
  int attempts = test_inline_static_attempts, sems = test_sem_creates,
      allocations = test_caller_allocations;
  int result = accept_path
                   ? esp_net_tcp_accept_managed(NULL, fd, &out, NULL, 0u)
                   : esp_net_tcp_connect_managed(NULL, fd, &address, 0u);
  assert(test_inline_static_attempts == attempts && test_sem_creates == sems &&
         test_caller_allocations == allocations);
  assert(result == (some_ready ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_WOULD_BLOCK));
  if (accept_path) assert(out == -1);
  esp_net_close(NULL, fd);
  test_wait_idle();
}
static void test_recovery_peer(int accept_path, test_call_t* call,
                               int* external_listener, int* peer) {
  *external_listener = *peer = -1;
  call->legacy = accept_path;
  if (accept_path) {
    assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                      &call->fd, &call->address) == H2_PAL_OK);
    *peer = socket(AF_INET, SOCK_STREAM, 0);
    assert(*peer >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_port = htons(call->address.port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(connect(*peer, (struct sockaddr*)&address, sizeof(address)) == 0);
  } else {
    assert(esp_net_tcp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, &call->fd) ==
           H2_PAL_OK);
    *external_listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(*external_listener >= 0);
    struct sockaddr_in local = {.sin_family = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(local);
    assert(bind(*external_listener, (struct sockaddr*)&local, size) == 0 &&
           listen(*external_listener, 1) == 0);
    assert(getsockname(*external_listener, (struct sockaddr*)&local, &size) ==
           0);
    call->address = (h2_pal_net_addr_t){.family = H2_PAL_NET_FAMILY_IPV4,
                                        .port = ntohs(local.sin_port),
                                        .ip = {127, 0, 0, 1}};
  }
}
static void* test_call_recovery_operation(void* context) {
  test_call_t* call = context;
  call->result = call->legacy
                     ? esp_net_tcp_accept_managed(NULL, call->fd, &call->out,
                                                  NULL, call->timeout_ms)
                     : esp_net_tcp_connect_managed(
                           NULL, call->fd, &call->address, call->timeout_ms);
  assert(pthread_mutex_lock(&test_lock) == 0);
  call->done = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return NULL;
}
static void test_concurrent_recovery_operation(int accept_path, int expires) {
  test_gate_begin(16);
  test_call_t initialization = {0};
  pthread_t initializer;
  assert(pthread_create(&initializer, NULL, test_call_native_init,
                        &initialization) == 0);
  test_gate_wait();
  test_call_t call = {
      .fd = -1, .out = 123, .timeout_ms = expires ? 20u : 1000u};
  int listener, peer;
  test_recovery_peer(accept_path, &call, &listener, &peer);
  int connects = test_native_connect_calls, accepts = test_native_accept_calls;
  pthread_t operation;
  assert(pthread_create(&operation, NULL, test_call_recovery_operation,
                        &call) == 0);
  if (expires) {
    assert(test_call_done(&call, 150u));
    assert(pthread_join(operation, NULL) == 0);
    assert(call.result == H2_PAL_ERR_TIMEOUT &&
           (!accept_path || call.out == -1));
    assert(test_native_connect_calls == connects &&
           test_native_accept_calls == accepts);
    test_gate_end();
    assert(pthread_join(initializer, NULL) == 0 && initialization.result);
  } else {
    test_pause_ms(20u);
    assert(!test_call_done(&call, 0u));
    test_gate_end();
    assert(pthread_join(initializer, NULL) == 0 && initialization.result);
    assert(pthread_join(operation, NULL) == 0 && call.result == H2_PAL_OK);
  }
  int fd = call.fd;
  if (!expires && accept_path) {
    esp_net_close(NULL, fd);
    fd = call.out;
  }
  if (!expires && !accept_path) {
    peer = accept(listener, NULL, NULL);
    assert(peer >= 0);
  }
  if (!expires) {
    assert(esp_net_tcp_send_timeout_managed(NULL, fd, (const uint8_t*)"OK", 2u,
                                            100u) == 2);
    char data[2];
    assert(recv(peer, data, 2u, 0) == 2 && memcmp(data, "OK", 2u) == 0);
    esp_net_close(NULL, fd);
    assert(recv(peer, data, 1u, 0) == 0);
    test_wait_closed(fd);
  } else
    esp_net_close(NULL, fd);
  if (peer >= 0) assert(close(peer) == 0);
  if (listener >= 0) assert(close(listener) == 0);
  assert(test_task_count == 4 && test_dynamic_task_calls == 0 &&
         test_caller_allocations == 0);
}

static void test_wait_initializing_call(int fd) {
  /* Baseline has no lease here; wait until the public operation reached the
   * held initializer, using a short scheduling opportunity before cancellation.
   */
  (void)fd;
  test_pause_ms(20u);
}
static void test_initialization_cancel_lease(int accept_path, int expires) {
  test_gate_begin(16);
  test_call_t initialization = {0};
  pthread_t initializer;
  assert(pthread_create(&initializer, NULL, test_call_native_init,
                        &initialization) == 0);
  test_gate_wait();
  test_call_t call = {
      .fd = -1, .out = 123, .timeout_ms = expires ? 20u : 1000u};
  int listener, peer;
  test_recovery_peer(accept_path, &call, &listener, &peer);
  int old_fd = call.fd;
  int connects = test_native_connect_calls, accepts = test_native_accept_calls;
  pthread_t operation;
  assert(pthread_create(&operation, NULL, test_call_recovery_operation,
                        &call) == 0);
  test_wait_initializing_call(old_fd);
  if (expires) {
    assert(test_call_done(&call, 150u));
    assert(pthread_join(operation, NULL) == 0);
    assert(call.result == H2_PAL_ERR_TIMEOUT &&
           (!accept_path || call.out == -1));
    esp_net_close(NULL, old_fd);
  } else {
    esp_net_close(NULL, old_fd);
    assert(test_call_done(&call,
                          150u)); /* Must cancel while init is still held. */
    assert(pthread_join(operation, NULL) == 0 &&
           call.result == H2_PAL_ERR_CLOSED &&
           (!accept_path || call.out == -1));
    test_wait_closed(old_fd);
  }
  /* Reuse the exact number before the old initializer is allowed to finish. */
  int replacement = socket(AF_INET, SOCK_STREAM, 0);
  assert(replacement >= 0);
  if (replacement != old_fd) {
    assert(dup2(replacement, old_fd) == old_fd);
    assert(close(replacement) == 0);
    replacement = old_fd;
  }
  assert(esp_net_socket_register(replacement, SOCK_STREAM,
                                 ESP_NET_SOCKET_FRESH) == H2_PAL_OK);
  test_gate_end();
  assert(pthread_join(initializer, NULL) == 0 && initialization.result);
  assert(test_native_connect_calls == connects &&
         test_native_accept_calls == accepts);
  struct sockaddr_storage address;
  socklen_t size = sizeof(address);
  assert(getpeername(replacement, (struct sockaddr*)&address, &size) < 0);
  esp_net_close(NULL, replacement);
  if (peer >= 0) assert(close(peer) == 0);
  if (listener >= 0) assert(close(listener) == 0);
}
static void test_poll_only_recovery(int accept_path, int admission_faults) {
  test_fail_task = -1;
  esp_net_tls_init();
  test_fail_task = 0;
  test_call_t call = {.fd = -1, .out = 123};
  int listener, peer;
  test_recovery_peer(accept_path, &call, &listener, &peer);
  int inline_attempts = test_inline_static_attempts,
      allocations = test_caller_allocations;
  if (admission_faults) {
    atomic_store(&test_scheduler_state, 0);
    assert(esp_net_tcp_connect_managed(NULL, call.fd, &call.address, 0u) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(test_deferred_enqueues == 0);
    atomic_store(&test_scheduler_state, taskSCHEDULER_RUNNING);
    assert(pthread_mutex_lock(&test_lock) == 0);
    test_daemon_hold = 1;
    test_fail_deferred_enqueue = 1;
    assert(pthread_mutex_unlock(&test_lock) == 0);
    assert(esp_net_tcp_connect_managed(NULL, call.fd, &call.address, 0u) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(test_deferred_enqueues == 0);
    for (int i = 0; i < 8; ++i)
      assert(esp_net_tcp_connect_managed(NULL, call.fd, &call.address, 0u) ==
             H2_PAL_ERR_WOULD_BLOCK);
    assert(test_deferred_enqueues == 1 &&
           test_inline_static_attempts == inline_attempts);
    assert(pthread_mutex_lock(&test_lock) == 0);
    test_daemon_hold = 0;
    assert(pthread_cond_broadcast(&test_changed) == 0);
    assert(pthread_mutex_unlock(&test_lock) == 0);
  }
  uint64_t until = esp_net_now_ms() + 1000u;
  while (!esp_net_socket_has_executor()) {
    int result =
        accept_path
            ? esp_net_tcp_accept_managed(NULL, call.fd, &call.out, NULL, 0u)
            : esp_net_tcp_connect_managed(NULL, call.fd, &call.address, 0u);
    assert(result == H2_PAL_ERR_WOULD_BLOCK);
    assert(test_inline_static_attempts == inline_attempts &&
           test_caller_allocations == allocations);
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  assert(test_deferred_enqueues > 0 && test_dynamic_task_calls == 0);
  /* Progress now comes only from zero-timeout calls; no getter or legacy TLS.
   */
  int result;
  do {
    result =
        accept_path
            ? esp_net_tcp_accept_managed(NULL, call.fd, &call.out, NULL, 0u)
            : esp_net_tcp_connect_managed(NULL, call.fd, &call.address, 0u);
    if (result == H2_PAL_ERR_TIMEOUT || result == H2_PAL_ERR_WOULD_BLOCK)
      test_pause_ms(1u);
    assert(esp_net_now_ms() < until);
  } while (result == H2_PAL_ERR_TIMEOUT || result == H2_PAL_ERR_WOULD_BLOCK);
  assert(result == H2_PAL_OK);
  int fd = call.fd;
  if (accept_path) {
    esp_net_close(NULL, fd);
    fd = call.out;
  } else {
    peer = accept(listener, NULL, NULL);
    assert(peer >= 0);
  }
  assert(esp_net_tcp_send_timeout_managed(NULL, fd, (const uint8_t*)"OK", 2u,
                                          100u) == 2);
  char bytes[2];
  assert(recv(peer, bytes, 2u, 0) == 2 && memcmp(bytes, "OK", 2u) == 0);
  esp_net_close(NULL, fd);
  assert(recv(peer, bytes, 1u, 0) == 0);
  assert(close(peer) == 0);
  if (listener >= 0) assert(close(listener) == 0);
  test_sdk_deferred_drain();
  test_wait_idle();
}
static void* test_short_native_init(void* context) {
  test_call_t* call = context;
  call->result = esp_net_native_init(esp_net_now_ms() + 10u);
  assert(pthread_mutex_lock(&test_lock) == 0);
  call->done = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return NULL;
}
static void test_long_waiter_retries(int accept_path) {
  test_gate_begin(16);
  test_call_t initialization = {0};
  pthread_t initializer;
  assert(pthread_create(&initializer, NULL, test_short_native_init,
                        &initialization) == 0);
  test_gate_wait();
  test_call_t call = {.fd = -1, .out = 123, .timeout_ms = 1000u};
  int listener, peer;
  test_recovery_peer(accept_path, &call, &listener, &peer);
  pthread_t operation;
  assert(pthread_create(&operation, NULL, test_call_recovery_operation,
                        &call) == 0);
  test_pause_ms(20u);
  test_gate_end();
  assert(pthread_join(initializer, NULL) == 0 && !initialization.result);
  assert(pthread_join(operation, NULL) == 0 && call.result == H2_PAL_OK);
  int fd = call.fd;
  if (accept_path) {
    esp_net_close(NULL, fd);
    fd = call.out;
  } else {
    peer = accept(listener, NULL, NULL);
    assert(peer >= 0);
  }
  assert(esp_net_tcp_send_timeout_managed(NULL, fd, (const uint8_t*)"OK", 2u,
                                          100u) == 2);
  char bytes[2];
  assert(recv(peer, bytes, 2u, 0) == 2);
  esp_net_close(NULL, fd);
  assert(recv(peer, bytes, 1u, 0) == 0);
  assert(close(peer) == 0);
  if (listener >= 0) assert(close(listener) == 0);
  assert(test_task_count == 4);
}
static void test_callback_races_waiter(int accept_path) {
  test_fail_task = -1;
  esp_net_tls_init();
  test_fail_task = 0;
  test_call_t call = {.fd = -1, .out = 123, .timeout_ms = 1000u};
  int listener, peer;
  test_recovery_peer(accept_path, &call, &listener, &peer);
  int attempts = test_static_task_attempts,
      inline_attempts = test_inline_static_attempts;
  test_gate_begin(16);
  int result =
      accept_path
          ? esp_net_tcp_accept_managed(NULL, call.fd, &call.out, NULL, 0u)
          : esp_net_tcp_connect_managed(NULL, call.fd, &call.address, 0u);
  assert(result == H2_PAL_ERR_WOULD_BLOCK);
  test_gate_wait();
  pthread_t operation;
  assert(pthread_create(&operation, NULL, test_call_recovery_operation,
                        &call) == 0);
  test_pause_ms(20u);
  assert(!test_call_done(&call, 0u));
  assert(test_static_task_attempts == attempts + 1);
  test_gate_end();
  assert(pthread_join(operation, NULL) == 0 && call.result == H2_PAL_OK);
  test_sdk_deferred_drain();
  assert(test_task_count == 4 && test_static_task_attempts == attempts + 4 &&
         test_inline_static_attempts == inline_attempts);
  assert(test_deferred_enqueues == 1 && test_deferred_executions == 1);
  int fd = call.fd;
  if (accept_path) {
    esp_net_close(NULL, fd);
    fd = call.out;
  } else {
    peer = accept(listener, NULL, NULL);
    assert(peer >= 0);
  }
  assert(esp_net_tcp_send_timeout_managed(NULL, fd, (const uint8_t*)"OK", 2u,
                                          100u) == 2);
  char bytes[2];
  assert(recv(peer, bytes, 2u, 0) == 2 && memcmp(bytes, "OK", 2u) == 0);
  esp_net_close(NULL, fd);
  assert(recv(peer, bytes, 1u, 0) == 0);
  assert(close(peer) == 0);
  if (listener >= 0) assert(close(listener) == 0);
  test_wait_idle();
}
static void test_waiter_owns_before_callback(void) {
  test_fail_task = -1;
  esp_net_tls_init();
  test_fail_task = 0;
  test_call_t call = {.fd = -1, .out = 123, .timeout_ms = 1000u};
  int listener, peer;
  test_recovery_peer(0, &call, &listener, &peer);
  int attempts = test_static_task_attempts;
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_daemon_hold = 1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(esp_net_tcp_connect_managed(NULL, call.fd, &call.address, 0u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  test_gate_begin(16);
  pthread_t operation;
  assert(pthread_create(&operation, NULL, test_call_recovery_operation,
                        &call) == 0);
  test_gate_wait();
  test_sdk_deferred_drain(); /* Callback arrives after the caller won creation.
                              */
  assert(test_deferred_executions == 1 &&
         test_static_task_attempts == attempts + 1);
  portENTER_CRITICAL(&s_esp_net_mux);
  assert(s_esp_native_initializing && !s_esp_net_native_ready);
  portEXIT_CRITICAL(&s_esp_net_mux);
  test_gate_end();
  assert(pthread_join(operation, NULL) == 0 && call.result == H2_PAL_OK);
  assert(test_task_count == 4 && test_static_task_attempts == attempts + 4);
  peer = accept(listener, NULL, NULL);
  assert(peer >= 0);
  assert(close(listener) == 0);
  assert(esp_net_tcp_send_timeout_managed(NULL, call.fd, (const uint8_t*)"OK",
                                          2u, 100u) == 2);
  char bytes[2];
  assert(recv(peer, bytes, 2u, 0) == 2 && memcmp(bytes, "OK", 2u) == 0);
  esp_net_close(NULL, call.fd);
  assert(recv(peer, bytes, 1u, 0) == 0);
  assert(close(peer) == 0);
  test_wait_idle();
}
static void test_all_sockets_close_before_callback(void) {
  test_fail_task = -1;
  esp_net_tls_init();
  test_fail_task = 0;
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_daemon_hold = 1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  h2_pal_net_socket_t fresh = -1, listener = -1, out = 123;
  h2_pal_net_addr_t address;
  assert(esp_net_tcp_open_managed(NULL, H2_PAL_NET_FAMILY_IPV4, &fresh) ==
         H2_PAL_OK);
  assert(esp_net_tcp_listen_managed(NULL, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                    &listener, &address) == H2_PAL_OK);
  assert(esp_net_tcp_connect_managed(NULL, fresh, &address, 0u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(esp_net_tcp_accept_managed(NULL, listener, &out, NULL, 0u) ==
             H2_PAL_ERR_WOULD_BLOCK &&
         out == -1);
  esp_net_close(NULL, fresh);
  esp_net_close(NULL, listener);
  assert(fcntl(fresh, F_GETFD) < 0 && fcntl(listener, F_GETFD) < 0);
  uint32_t generations[CONFIG_LWIP_MAX_SOCKETS];
  portENTER_CRITICAL(&s_esp_net_mux);
  for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
    assert(!s_esp_net_records[i].used);
    generations[i] = s_esp_net_records[i].generation;
  }
  portEXIT_CRITICAL(&s_esp_net_mux);
  int closed = test_closed, connects = test_native_connect_calls,
      accepts = test_native_accept_calls, allocations = test_caller_allocations;
  test_sdk_deferred_drain();
  assert(esp_net_socket_has_executor() && test_task_count == 4 &&
         test_deferred_executions == 1);
  assert(test_closed == closed && test_native_connect_calls == connects &&
         test_native_accept_calls == accepts &&
         test_caller_allocations == allocations);
  portENTER_CRITICAL(&s_esp_net_mux);
  for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS; ++i)
    assert(!s_esp_net_records[i].used &&
           generations[i] == s_esp_net_records[i].generation);
  portEXIT_CRITICAL(&s_esp_net_mux);
  test_wait_idle();
}

int main(int argc, char** argv) {
  test_sdk_runtime_start();
  if (argc == 2) {
    signal(SIGPIPE, SIG_IGN);
    if (strcmp(argv[1], "waiter-before-callback") == 0) {
      test_waiter_owns_before_callback();
    } else if (strcmp(argv[1], "callback-waiter-connect") == 0 ||
               strcmp(argv[1], "callback-waiter-accept") == 0) {
      test_callback_races_waiter(strcmp(argv[1], "callback-waiter-accept") ==
                                 0);
    } else if (strcmp(argv[1], "callback-after-all-close") == 0) {
      test_all_sockets_close_before_callback();
    } else if (strcmp(argv[1], "init-cancel-connect") == 0 ||
               strcmp(argv[1], "init-cancel-accept") == 0 ||
               strcmp(argv[1], "init-lease-deadline") == 0) {
      test_initialization_cancel_lease(
          strcmp(argv[1], "init-cancel-connect") != 0,
          strcmp(argv[1], "init-lease-deadline") == 0);
    } else if (strcmp(argv[1], "poll-only-connect") == 0 ||
               strcmp(argv[1], "poll-only-accept") == 0) {
      test_poll_only_recovery(strcmp(argv[1], "poll-only-accept") == 0, 0);
    } else if (strcmp(argv[1], "poll-admission-retry") == 0) {
      test_poll_only_recovery(0, 1);
    } else if (strcmp(argv[1], "long-waiter-connect") == 0 ||
               strcmp(argv[1], "long-waiter-accept") == 0) {
      test_long_waiter_retries(strcmp(argv[1], "long-waiter-accept") == 0);
    } else if (strcmp(argv[1], "zero-connect-recovery") == 0 ||
               strcmp(argv[1], "zero-accept-recovery") == 0 ||
               strcmp(argv[1], "zero-ready-poll") == 0) {
      test_zero_timeout_recovery(strcmp(argv[1], "zero-connect-recovery") != 0,
                                 strcmp(argv[1], "zero-ready-poll") == 0);
    } else if (strcmp(argv[1], "concurrent-connect-recovery") == 0 ||
               strcmp(argv[1], "concurrent-accept-recovery") == 0 ||
               strcmp(argv[1], "concurrent-recovery-deadline") == 0) {
      test_concurrent_recovery_operation(
          strcmp(argv[1], "concurrent-connect-recovery") != 0,
          strcmp(argv[1], "concurrent-recovery-deadline") == 0);
    } else if (strcmp(argv[1], "plain-connect-retry") == 0 ||
               strcmp(argv[1], "plain-accept-retry") == 0) {
      test_retained_plain_tcp_recovery(
          strcmp(argv[1], "plain-accept-retry") == 0, 0);
    } else if (strcmp(argv[1], "plain-connect-no-tls-heap") == 0 ||
               strcmp(argv[1], "plain-accept-no-tls-heap") == 0) {
      test_retained_plain_tcp_recovery(
          strcmp(argv[1], "plain-accept-no-tls-heap") == 0, 1);
    } else if (strcmp(argv[1], "native-init-budget") == 0) {
      test_native_init_budget();
    } else if (strcmp(argv[1], "native-init-concurrent") == 0) {
      test_native_init_concurrent();
    } else if (strcmp(argv[1], "legacy-retry") == 0 ||
               strcmp(argv[1], "legacy-sync-retry") == 0) {
      test_retained_legacy_api_retry(strcmp(argv[1], "legacy-sync-retry") == 0);
    } else if (strcmp(argv[1], "prepare-no-reaper") == 0) {
      test_fail_task = 1;
      esp_net_tls_init();
      test_prepare_failure_without_reaper(1);
      test_prepare_failure_without_reaper(3);
    } else if (strcmp(argv[1], "cancel-no-reaper") == 0) {
      test_fail_task = 1;
      esp_net_tls_init();
      test_fail_task = -1;
      test_cancel_without_reaper();
    } else {
      esp_net_tls_init();
      if (strcmp(argv[1], "write-cancel-plain") == 0)
        test_write_cancel_during_other_graceful_close(0, 0);
      else if (strcmp(argv[1], "write-cancel-tls") == 0)
        test_write_cancel_during_other_graceful_close(1, 0);
      else if (strcmp(argv[1], "write-cancel-bounded") == 0)
        test_write_cancel_during_other_graceful_close(0, 1);
      else if (strcmp(argv[1], "ordinary-send-cancel") == 0)
        test_ordinary_send_cancel();
      else if (strcmp(argv[1], "retry-next-tick") == 0)
        test_retry_after_early_tick();
      else if (strcmp(argv[1], "cancel-during-fin") == 0)
        test_receive_cancel_during_other_graceful_close();
      else if (strcmp(argv[1], "accept-close-handoff") == 0)
        test_unpublished_accept_cleanup(0);
      else if (strcmp(argv[1], "accept-adopt-handoff") == 0)
        test_unpublished_accept_cleanup(1);
      else if (strcmp(argv[1], "graceful-deadline") == 0)
        test_graceful_close_does_not_block_caller();
      else if (strcmp(argv[1], "single-stalled-free") == 0)
        test_one_stalled_destructor_does_not_exhaust_slots();
      else if (strcmp(argv[1], "completed-io") == 0)
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
    test_sdk_report();
    return 0;
  }

  (void)s_esp_resolver_count;
  signal(SIGPIPE, SIG_IGN);
  test_all_executors_unavailable();
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
  test_graceful_close_does_not_block_caller();
  test_one_stalled_destructor_does_not_exhaust_slots();
  for (int fault = 0; fault <= 3; ++fault)
    test_unpublished_accept_cleanup(fault);
  test_accept_capacity_reserved_before_native();
  test_accept_empty_poll_and_negative_ticket();
  test_receive_cancel_during_other_graceful_close();
  test_queued_cancel_on_native_busy_transition();
  test_write_cancel_during_other_graceful_close(0, 0);
  test_write_cancel_during_other_graceful_close(1, 0);
  test_write_cancel_during_other_graceful_close(0, 1);
  test_tls_write_retry_prefix_and_close();
  test_ordinary_send_cancel();
  test_retry_after_early_tick();
  assert(test_task_count == 4);
  test_sdk_report();
  puts(
      "ESP production TLS deadlines, initialized resource pool, large CA, close/pins/generations, concurrent wraps and all socket entries PASS");
  return 0;
}
