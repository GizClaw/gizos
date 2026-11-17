/* Only the provider's send boundary is substituted by the test target. */
#undef send
#include "h2_posix_pal_core.h"

#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ms(void) {
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void make_stream_pair(int pair[2]) {
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  int size = 4096;
  assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
}

static void fill_send_buffer(const h2_pal_net_api_t* net,
                             h2_pal_net_socket_t socket_fd) {
  uint8_t data[4096] = {0};
  for (;;) {
    int result =
        h2_pal_net_tcp_send_timeout(net, socket_fd, data, sizeof(data), 0u);
    if (result == H2_PAL_ERR_WOULD_BLOCK) {
      return;
    }
    assert(result > 0);
  }
}

static pthread_mutex_t signal_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t signal_changed = PTHREAD_COND_INITIALIZER;
static int inject_signal, send_entered, signal_sent;
static atomic_uint handled_signals;

static void count_signal(int number) {
  if (number == SIGPIPE)
    atomic_fetch_add_explicit(&handled_signals, 1u, memory_order_relaxed);
}

static void wait_for_signal_flag(const int* flag) {
  struct timespec until;
  assert(clock_gettime(CLOCK_REALTIME, &until) == 0);
  until.tv_sec += 5;
  while (!*flag)
    assert(pthread_cond_timedwait(&signal_changed, &signal_lock, &until) == 0);
}

ssize_t h2_test_net_send(int fd, const void* bytes, size_t len, int flags) {
  assert(pthread_mutex_lock(&signal_lock) == 0);
  if (inject_signal) {
    send_entered = 1;
    assert(pthread_cond_broadcast(&signal_changed) == 0);
    wait_for_signal_flag(&signal_sent);
  }
  assert(pthread_mutex_unlock(&signal_lock) == 0);
  return send(fd, bytes, len, flags);
}

static void* send_unrelated_signal(void* ignored) {
  (void)ignored;
  sigset_t mask;
  assert(sigemptyset(&mask) == 0 && sigaddset(&mask, SIGPIPE) == 0);
  assert(pthread_sigmask(SIG_BLOCK, &mask, NULL) == 0);
  assert(pthread_mutex_lock(&signal_lock) == 0);
  wait_for_signal_flag(&send_entered);
  assert(kill(getpid(), SIGPIPE) == 0);
  signal_sent = 1;
  assert(pthread_cond_broadcast(&signal_changed) == 0);
  assert(pthread_mutex_unlock(&signal_lock) == 0);
  return NULL;
}

static void unrelated_signal_is_preserved(const h2_pal_net_api_t* net,
                                          int timed, int blocked) {
  assert(atomic_is_lock_free(&handled_signals));
  atomic_store_explicit(&handled_signals, 0u, memory_order_relaxed);
  struct sigaction action = {.sa_handler = count_signal}, previous_action;
  assert(sigemptyset(&action.sa_mask) == 0);
  assert(sigaction(SIGPIPE, &action, &previous_action) == 0);
  sigset_t pipe_mask, previous_mask, after_mask, pending;
  assert(sigemptyset(&pipe_mask) == 0 && sigaddset(&pipe_mask, SIGPIPE) == 0);
  assert(pthread_sigmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &pipe_mask,
                         &previous_mask) == 0);
  int pair[2];
  make_stream_pair(pair);
  inject_signal = 1;
  send_entered = signal_sent = 0;
  pthread_t sender;
  assert(pthread_create(&sender, NULL, send_unrelated_signal, NULL) == 0);
  const uint8_t byte = 0x42;
  uint64_t started = monotonic_ms();
  int result = timed
                   ? h2_pal_net_tcp_send_timeout(net, pair[0], &byte, 1u, 100u)
                   : h2_pal_net_tcp_send(net, pair[0], &byte, 1u);
  assert(result == 1 && monotonic_ms() - started < 1000u);
  assert(pthread_join(sender, NULL) == 0);
  inject_signal = 0;
  assert(pthread_sigmask(SIG_BLOCK, NULL, &after_mask) == 0);
  assert(sigismember(&after_mask, SIGPIPE) == blocked);
  assert(sigpending(&pending) == 0);
  assert(atomic_load_explicit(&handled_signals, memory_order_relaxed) == 1u ||
         sigismember(&pending, SIGPIPE) == 1);
  /* Deliver only this test's pending signal through its handler, without a
   * sigpending/sigwait race with another signal-consuming thread. */
  assert(pthread_sigmask(SIG_UNBLOCK, &pipe_mask, NULL) == 0);
  const uint64_t delivery_deadline = monotonic_ms() + 1000u;
  while (atomic_load_explicit(&handled_signals, memory_order_relaxed) == 0u &&
         monotonic_ms() < delivery_deadline) {
    const struct timespec pause = {.tv_nsec = 1000000L};
    (void)nanosleep(&pause, NULL);
  }
  assert(atomic_load_explicit(&handled_signals, memory_order_relaxed) == 1u);
  uint8_t received = 0;
  assert(recv(pair[1], &received, 1u, 0) == 1 && received == byte);
  close(pair[0]);
  close(pair[1]);
  assert(pthread_sigmask(SIG_SETMASK, &previous_mask, NULL) == 0);
  assert(sigaction(SIGPIPE, &previous_action, NULL) == 0);
}

static void closed_send_preserves_signals(const h2_pal_net_api_t* net,
                                          int timed, int pending_before) {
  sigset_t pipe_mask, original_mask, pending, after_mask;
  assert(sigemptyset(&pipe_mask) == 0);
  assert(sigaddset(&pipe_mask, SIGPIPE) == 0);
  assert(pthread_sigmask(SIG_BLOCK, &pipe_mask, &original_mask) == 0);
  assert(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 0);
  if (pending_before) assert(raise(SIGPIPE) == 0);
  int pair[2];
  make_stream_pair(pair);
  close(pair[1]);
  const uint8_t byte = 0u;
  int result = timed
                   ? h2_pal_net_tcp_send_timeout(net, pair[0], &byte, 1u, 100u)
                   : h2_pal_net_tcp_send(net, pair[0], &byte, 1u);
  assert(result == H2_PAL_ERR_CLOSED);
  assert(pthread_sigmask(SIG_BLOCK, NULL, &after_mask) == 0);
  assert(sigismember(&after_mask, SIGPIPE) == 1);
  assert(sigpending(&pending) == 0);
  assert(sigismember(&pending, SIGPIPE) == pending_before);
  if (pending_before) {
    int signal_number = 0;
    assert(sigwait(&pipe_mask, &signal_number) == 0 &&
           signal_number == SIGPIPE);
  }
  close(pair[0]);
  assert(pthread_sigmask(SIG_SETMASK, &original_mask, NULL) == 0);
}

int main(void) {
  assert(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
  const h2_pal_net_api_t* net = h2_posix_net_api();
  for (int timed = 0; timed < 2; ++timed) {
    for (int pending = 0; pending < 2; ++pending)
      closed_send_preserves_signals(net, timed, pending);
    for (int blocked = 0; blocked < 2; ++blocked)
      unrelated_signal_is_preserved(net, timed, blocked);
  }
  assert(net != NULL && net->vtable != NULL &&
         net->vtable->tcp_send_timeout != NULL);

  h2_pal_net_addr_t missing = {0};
  assert(h2_pal_net_get_host_addr(net, "h2-e2e-missing-interface", &missing) ==
         H2_PAL_ERR_NOT_FOUND);

  int pair[2];
  make_stream_pair(pair);
  size_t payload_len = 1024u * 1024u;
  uint8_t* payload = malloc(payload_len);
  assert(payload != NULL);
  memset(payload, 0x5a, payload_len);
  int partial =
      h2_pal_net_tcp_send_timeout(net, pair[0], payload, payload_len, 100u);
  assert(partial > 0 && (size_t)partial < payload_len);
  fill_send_buffer(net, pair[0]);
  assert(h2_pal_net_tcp_send_timeout(net, pair[0], payload, 1u, 0u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  uint64_t started_ms = monotonic_ms();
  assert(h2_pal_net_tcp_send_timeout(net, pair[0], payload, 1u, 30u) ==
         H2_PAL_ERR_TIMEOUT);
  uint64_t elapsed_ms = monotonic_ms() - started_ms;
  assert(elapsed_ms >= 20u && elapsed_ms < 500u);
  close(pair[1]);
  assert(h2_pal_net_tcp_send_timeout(net, pair[0], payload, 1u, 100u) ==
         H2_PAL_ERR_CLOSED);
  close(pair[0]);

  make_stream_pair(pair);
  close(pair[1]);
  uint8_t byte = 0u;
  assert(h2_pal_net_tcp_recv(net, pair[0], &byte, 1u, 100u) ==
         H2_PAL_ERR_CLOSED);
  close(pair[0]);
  free(payload);
  return 0;
}
