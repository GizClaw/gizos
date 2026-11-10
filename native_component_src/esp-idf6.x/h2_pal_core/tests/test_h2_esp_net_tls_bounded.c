/* Compiled with the production Net socket operations and TLS implementation. */
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
  esp_net_tls_init();
  assert(s_esp_tls_initialized && s_esp_tls_pool_ready && test_task_count == 3);
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

int main(void) {
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
  assert(test_task_count == 3);
  puts(
      "ESP production TLS deadlines, initialized resource pool, large CA, close/pins/generations, concurrent wraps and all socket entries PASS");
  return 0;
}
