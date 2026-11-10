/* Appended to the real Net TLS implementation by the owner host harness. */
static const h2_pal_net_tls_config_t test_config = {
    .server_name = "h2.example",
    .alpn = "h2",
    .root_ca_pem = (const uint8_t*)"ROOT",
    .root_ca_pem_len = 4u,
    .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
};

static void test_pause_ms(unsigned ms) {
  struct timespec pause = {ms / 1000u, (long)(ms % 1000u) * 1000000};
  assert(nanosleep(&pause, NULL) == 0);
}

static void test_wait_idle(void) {
  uint64_t until = esp_net_now_ms() + 3000u;
  for (;;) {
    assert(xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY));
    int busy = 0;
    for (size_t i = 0; i < H2_ESP_NET_TLS_WORKER_MAX; ++i)
      busy |= s_esp_tls_workers[i].job != NULL;
    assert(xSemaphoreGive(s_esp_tls_sync->registry_mutex));
    if (!busy) break;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  assert(pthread_mutex_lock(&test_lock) == 0);
  assert(test_allocations == 1 && test_contexts == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  for (size_t i = 0; i < H2_ESP_NET_TLS_SOCKET_MAX; ++i) {
    assert(s_esp_tls_sockets[i].state == ESP_NET_TLS_SOCKET_FREE);
    assert(s_esp_tls_sockets[i].fd == -1);
  }
}

static void test_pair(int pair[2]) {
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(send(pair[1], "WORLD", 5u, 0) == 5);
}

static void test_success(int pair[2], int bounded) {
  h2_pal_net_socket_t out = -2;
  assert((bounded ? esp_net_tls_wrap_bounded : esp_net_tls_wrap)(
             NULL, pair[0], &test_config, 1000u, &out) == H2_PAL_OK);
  assert(out == pair[0]);
  char message[5];
  assert(recv(pair[1], message, sizeof(message), 0) == 5);
  assert(memcmp(message, "HELLO", 5u) == 0);
  int index = esp_net_tls_find(out);
  assert(index >= 0);
  esp_net_tls_socket_t* slot = &s_esp_tls_sockets[index];
  assert(slot->ssl.bio == slot && slot->ssl.config == &slot->config);
  assert(slot->config.alpn == slot->alpn &&
         slot->alpn[0] == slot->alpn_storage[0]);
  assert(slot->config.ca == &slot->ca);
  esp_net_tls_socket_t* acquired = NULL;
  size_t acquired_index = SIZE_MAX;
  assert(esp_net_tls_acquire(out, 1, 100u, esp_net_now_ms() + 100u, &acquired,
                             &acquired_index) == 1);
  assert(acquired == slot && acquired_index == (size_t)index);
  assert(esp_net_tls_recv_finish(slot, acquired_index, esp_net_now_ms(), 7) ==
         7);
  esp_net_close(NULL, out);
  assert(close(pair[1]) == 0);
}

static void test_failures_before_start(void) {
  int pair[2];
  test_pair(pair);
  h2_pal_net_socket_t out = 123;
  test_fail_task = 1;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 100u, &out) ==
         H2_PAL_ERR_TASK);
  assert(out == -1 && test_task_count == 0);
  test_wait_idle();
  test_alloc_fail = 1;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 100u, &out) ==
         H2_PAL_ERR_NO_MEMORY);
  test_wait_idle();
  assert(xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY));
  uint64_t start = esp_net_now_ms();
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 40u, &out) ==
         H2_PAL_ERR_TIMEOUT);
  assert(esp_net_now_ms() - start < 400u && out == -1);
  /* The registry remains blocked when coreHTTP closes the failed raw fd. */
  start = esp_net_now_ms();
  esp_net_close(NULL, pair[0]);
  assert(esp_net_now_ms() - start < 100u);
  assert(xSemaphoreGive(s_esp_tls_sync->registry_mutex));
  assert(close(pair[1]) == 0);
  test_wait_idle();

  h2_pal_net_tls_config_t config = test_config;
  config.root_ca_pem_len = H2_ESP_NET_TLS_CA_MAX + 1u;
  assert(esp_net_tls_wrap_bounded(NULL, 5, &config, 100u, &out) ==
         H2_PAL_ERR_NO_SPACE);
  char name[H2_ESP_NET_TLS_SERVER_NAME_MAX + 2u];
  memset(name, 'a', sizeof(name));
  name[sizeof(name) - 1u] = '\0';
  config = test_config;
  config.server_name = name;
  assert(esp_net_tls_wrap_bounded(NULL, 5, &config, 100u, &out) ==
         H2_PAL_ERR_NO_SPACE);
  config = test_config;
  config.alpn = "0123456789abcdef";
  assert(esp_net_tls_wrap_bounded(NULL, 5, &config, 100u, &out) ==
         H2_PAL_ERR_NO_SPACE);
  config = test_config;
  config.root_ca_pem = NULL;
  assert(esp_net_tls_wrap_bounded(NULL, 5, &config, 100u, &out) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(esp_net_tls_wrap_bounded(NULL, 5, &test_config, 0u, &out) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(esp_net_tls_timeout_ticks(UINT32_MAX) != portMAX_DELAY);
}

static void test_stalled_sdk_and_fd_reuse(int stage) {
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_stage = stage;
  test_gate_release = test_gate_entered = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  h2_pal_net_tls_config_t* config = malloc(sizeof(*config));
  *config = test_config;
  char* name = strdup("h2.example");
  char* alpn = strdup("h2");
  char* ca = strdup("ROOT");
  assert(config && name && alpn && ca);
  config->server_name = name;
  config->alpn = alpn;
  config->root_ca_pem = stage == 4 ? NULL : (const uint8_t*)ca;
  config->root_ca_pem_len = stage == 4 ? 0u : 4u;
  int pair[2];
  test_pair(pair);
  int original_fd = pair[0];
  h2_pal_net_socket_t out = 123;
  uint64_t start = esp_net_now_ms();
  assert(esp_net_tls_wrap_bounded(NULL, original_fd, config, 60u, &out) ==
         H2_PAL_ERR_TIMEOUT);
  assert(out == -1 && esp_net_now_ms() - start < 500u);
  assert(pthread_mutex_lock(&test_lock) == 0);
  assert(test_gate_entered == 1);
  /* The first SDK call stays blocked, but later jobs can use the other worker.
   */
  test_gate_stage = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  memset(name, 'x', 10u);
  memset(alpn, 'x', 2u);
  memset(ca, 'x', 4u);
  memset(config, 0xa5, sizeof(*config));
  free(name);
  free(alpn);
  free(ca);
  free(config);
  start = esp_net_now_ms();
  esp_net_close(NULL, original_fd);
  assert(esp_net_now_ms() - start < 100u);
  assert(close(pair[1]) == 0);
  int reused[2];
  test_pair(reused);
  if (reused[0] != original_fd) {
    assert(dup2(reused[0], original_fd) == original_fd);
    assert(close(reused[0]) == 0);
    reused[0] = original_fd;
  }
  test_success(reused, 1);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_release = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_wait_idle();
  /* Reuse the original slot after the stale completion has been reclaimed. */
  test_pair(reused);
  test_success(reused, 1);
  test_wait_idle();
}

static void test_capacity_and_error_cleanup(void) {
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_stage = 1;
  test_gate_release = test_gate_entered = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  for (unsigned i = 0; i < H2_ESP_NET_TLS_WORKER_MAX; ++i) {
    int pair[2];
    test_pair(pair);
    h2_pal_net_socket_t out = -2;
    assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 40u, &out) ==
           H2_PAL_ERR_TIMEOUT);
    assert(out == -1);
    esp_net_close(NULL, pair[0]);
    assert(close(pair[1]) == 0);
  }
  h2_pal_net_socket_t out = 0;
  assert(esp_net_tls_wrap_bounded(NULL, 5, &test_config, 1000u, &out) ==
         H2_PAL_ERR_NO_SPACE);
  assert(out == -1);
  assert(pthread_mutex_lock(&test_lock) == 0);
  assert(test_gate_entered == (int)H2_ESP_NET_TLS_WORKER_MAX);
  test_gate_release = 1;
  test_gate_stage = 0;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_wait_idle();
  int pair[2];
  test_pair(pair);
  test_parse_fail = 1;
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 1000u, &out) ==
         H2_PAL_ERR_FORMAT);
  esp_net_close(NULL, pair[0]);
  assert(close(pair[1]) == 0);
  test_wait_idle();
  test_parse_fail = 0;

  /* A handshake whose BIO needs unavailable network data shares the budget. */
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  uint64_t start = esp_net_now_ms();
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 40u, &out) ==
         H2_PAL_ERR_TIMEOUT);
  assert(esp_net_now_ms() - start < 400u);
  esp_net_close(NULL, pair[0]);
  assert(close(pair[1]) == 0);
  test_wait_idle();
}

static void test_publish_lock_deadline(void) {
  int pair[2];
  test_pair(pair);
  h2_pal_net_socket_t out = 0;
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_publish_block = 1;
  test_publish_release = test_publish_held = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  uint64_t start = esp_net_now_ms();
  assert(esp_net_tls_wrap_bounded(NULL, pair[0], &test_config, 100u, &out) ==
         H2_PAL_ERR_TIMEOUT);
  assert(esp_net_now_ms() - start < 500u && out == -1);
  start = esp_net_now_ms();
  esp_net_close(NULL, pair[0]);
  assert(esp_net_now_ms() - start < 100u);
  assert(close(pair[1]) == 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  assert(test_publish_held);
  test_publish_release = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(pthread_join(test_publish_thread, NULL) == 0);
  test_wait_idle();
}

static int test_legacy_done;
static void* test_legacy_call(void* user) {
  int* pair = user;
  h2_pal_net_socket_t out = 0;
  assert(esp_net_tls_wrap(NULL, pair[0], &test_config, 20u, &out) ==
         H2_PAL_ERR_TIMEOUT);
  assert(out == -1);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_legacy_done = 1;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  return NULL;
}
static void test_legacy_compatibility(void) {
  int pair[2];
  test_pair(pair);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_stage = 2;
  test_gate_entered = test_gate_release = test_legacy_done = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  pthread_t legacy;
  assert(pthread_create(&legacy, NULL, test_legacy_call, pair) == 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  while (!test_gate_entered)
    assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  test_pause_ms(40u);
  assert(pthread_mutex_lock(&test_lock) == 0);
  /* The legacy entry retains synchronous, non-preemptible setup semantics. */
  assert(!test_legacy_done);
  test_gate_release = 1;
  test_gate_stage = 0;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(pthread_join(legacy, NULL) == 0);
  esp_net_close(NULL, pair[0]);
  assert(close(pair[1]) == 0);
  test_wait_idle();
  test_pair(pair);
  test_success(pair, 0);
  test_wait_idle();
}

static void test_accepted_cleanup_and_slot_reuse(void) {
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_stage = 6;
  test_gate_release = test_gate_entered = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  int pair[2];
  test_pair(pair);
  int original_fd = pair[0];
  test_success(pair, 1);
  assert(pthread_mutex_lock(&test_lock) == 0);
  while (!test_gate_entered)
    assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
  test_gate_stage = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  /* The accepted job still owns its cleanup resources, while close has
   * freed its TLS slot. A new job may reuse that slot and fd immediately. */
  int reused[2];
  test_pair(reused);
  if (reused[0] != original_fd) {
    assert(dup2(reused[0], original_fd) == original_fd);
    assert(close(reused[0]) == 0);
    reused[0] = original_fd;
  }
  h2_pal_net_socket_t out = -1;
  assert(esp_net_tls_wrap_bounded(NULL, original_fd, &test_config, 1000u,
                                  &out) == H2_PAL_OK);
  int index = esp_net_tls_find(out);
  assert(index >= 0);
  uint32_t generation = s_esp_tls_sync->generations[index];
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_release = 1;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  /* Wait for both jobs without requiring the live replacement slot to free. */
  uint64_t until = esp_net_now_ms() + 3000u;
  for (;;) {
    assert(xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY));
    int busy = 0;
    for (size_t i = 0; i < H2_ESP_NET_TLS_WORKER_MAX; ++i)
      busy |= s_esp_tls_workers[i].job != NULL;
    assert(xSemaphoreGive(s_esp_tls_sync->registry_mutex));
    if (!busy) break;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  assert(esp_net_tls_has_identity(out));
  assert(s_esp_tls_sync->generations[index] == generation);
  assert(s_esp_tls_sockets[index].state == ESP_NET_TLS_SOCKET_ACTIVE);
  assert(s_esp_tls_sockets[index].ssl.self == &s_esp_tls_sockets[index].ssl);
  esp_net_close(NULL, out);
  assert(close(reused[1]) == 0);
  test_wait_idle();
}

static void* test_legacy_close_wrap(void* user) {
  int fd = *(int*)user;
  h2_pal_net_socket_t out = -1;
  assert(esp_net_tls_wrap(NULL, fd, &test_config, 1000u, &out) ==
         H2_PAL_ERR_CLOSED);
  assert(out == -1);
  return NULL;
}
static void* test_legacy_closer(void* user) {
  esp_net_close(NULL, *(int*)user);
  return NULL;
}
static void test_legacy_identity_lifetime(void) {
  int pair[2];
  test_pair(pair);
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_stage = 2;
  test_gate_release = test_gate_entered = 0;
  assert(pthread_mutex_unlock(&test_lock) == 0);
  pthread_t wrapper, closer;
  assert(pthread_create(&wrapper, NULL, test_legacy_close_wrap, &pair[0]) == 0);
  assert(pthread_mutex_lock(&test_lock) == 0);
  while (!test_gate_entered)
    assert(pthread_cond_wait(&test_changed, &test_lock) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(esp_net_tls_has_identity(pair[0]));
  assert(pthread_create(&closer, NULL, test_legacy_closer, &pair[0]) == 0);
  uint64_t until = esp_net_now_ms() + 1000u;
  for (;;) {
    assert(xSemaphoreTake(s_esp_tls_sync->registry_mutex, portMAX_DELAY));
    int index = esp_net_tls_find(pair[0]);
    assert(index >= 0);
    int closing = s_esp_tls_sockets[index].state == ESP_NET_TLS_SOCKET_CLOSING;
    assert(xSemaphoreGive(s_esp_tls_sync->registry_mutex));
    if (closing) break;
    assert(esp_net_now_ms() < until);
    test_pause_ms(1u);
  }
  /* close still serializes with configuring legacy I/O, and its identity
   * stays present until context destruction and generation invalidation. */
  assert(esp_net_tls_has_identity(pair[0]));
  assert(pthread_mutex_lock(&test_lock) == 0);
  test_gate_release = 1;
  test_gate_stage = 0;
  assert(pthread_cond_broadcast(&test_changed) == 0);
  assert(pthread_mutex_unlock(&test_lock) == 0);
  assert(pthread_join(wrapper, NULL) == 0);
  assert(pthread_join(closer, NULL) == 0);
  assert(!esp_net_tls_has_identity(pair[0]));
  assert(close(pair[1]) == 0);
  test_wait_idle();
}

int main(void) {
  (void)s_esp_resolver_count;
  esp_net_tls_init();
  assert(s_esp_tls_sync != NULL);
  test_failures_before_start();
  for (int stage = 1; stage <= 5; ++stage) test_stalled_sdk_and_fd_reuse(stage);
  test_capacity_and_error_cleanup();
  test_publish_lock_deadline();
  test_legacy_compatibility();
  test_accepted_cleanup_and_slot_reuse();
  test_legacy_identity_lifetime();
  assert(test_task_count == (int)H2_ESP_NET_TLS_WORKER_MAX);
  assert(test_io_calls > 0 && test_closed > 0);
  assert(test_ca_checks > 0 && test_alpn_checks > 0 && test_sni_checks > 0);
  puts(
      "ESP production TLS setup/handshake/lock deadline, ownership, fd and slot reuse, capacity, cleanup and legacy cases PASS");
  return 0;
}
