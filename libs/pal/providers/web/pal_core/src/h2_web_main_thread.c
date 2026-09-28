#include "h2_web_main_thread.h"
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

/* Context pointer arrays and result storage use the wasm32 ABI. Scalar
 * values stay in typed C objects; doubles and uint64_t are never cast to ints.
 */
_Static_assert(sizeof(void *) == 4, "Web bridge requires wasm32 pointers");
_Static_assert(sizeof(h2_web_main_result_t) == 8,
               "Web result is one scalar slot");
EM_JS_DEPS(h2_web_main_context, "$h2WebMain");

static pthread_once_t once = PTHREAD_ONCE_INIT;
static em_proxying_queue *queue;
static void create_queue(void) {
  queue = em_proxying_queue_create();
  if (queue == NULL)
    abort();
}
struct h2_web_main_completion {
  void (*callback)(void *, h2_web_main_result_t *, h2_web_main_completion_t *);
  h2_web_main_result_t result;
  void *context;
  em_proxying_ctx *proxy;
  bool finished;
};

static void dispatch_main_call(em_proxying_ctx *proxy, void *context) {
  h2_web_main_completion_t *call = context;
  call->proxy = proxy;
  call->callback(call->context, &call->result, call);
}

EMSCRIPTEN_KEEPALIVE void h2_web_main_complete(h2_web_main_completion_t *call) {
  if (call == NULL || call->finished)
    abort();
  call->finished = true;
  em_proxying_ctx *proxy = call->proxy;
  if (proxy != NULL)
    emscripten_proxy_finish(proxy);
}

h2_web_main_result_t
h2_web_main_call(void (*callback)(void *, h2_web_main_result_t *,
                                  h2_web_main_completion_t *),
                 void *context) {
  if (callback == NULL)
    abort();
  h2_web_main_completion_t call = {.callback = callback, .context = context};
  if (emscripten_is_main_runtime_thread()) {
    dispatch_main_call(NULL, &call);
    if (!call.finished)
      abort();
    return call.result;
  }
  unsigned depth = h2_web_state_pause();
  pthread_once(&once, create_queue);
  if (!emscripten_proxy_sync_with_ctx(queue,
                                      emscripten_main_runtime_thread_id(),
                                      dispatch_main_call, &call))
    abort();
  h2_web_state_resume(depth);
  return call.result;
}

/* Browser handle registries are shared by Worker calls and UI completions.
 * Nesting is tracked per thread so a Promise wait can release the entire
 * admission lock without leaving a recursive mutex partly held. */
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned state_depth;
static pthread_cond_t state_changed = PTHREAD_COND_INITIALIZER;
void h2_web_state_enter(void) {
  if (state_depth++ == 0)
    pthread_mutex_lock(&state_mutex);
}
void h2_web_state_leave(int *guard) {
  (void)guard;
  if (--state_depth == 0)
    pthread_mutex_unlock(&state_mutex);
}
unsigned h2_web_state_pause(void) {
  unsigned depth = state_depth;
  state_depth = 0;
  if (depth)
    pthread_mutex_unlock(&state_mutex);
  return depth;
}
void h2_web_state_resume(unsigned depth) {
  if (depth)
    pthread_mutex_lock(&state_mutex);
  state_depth = depth;
}

void h2_web_worker_sleep(uint32_t ms) {
  if (emscripten_is_main_runtime_thread())
    abort();
  unsigned depth = h2_web_state_pause();
  /* A zero-duration test pause still gives the browser event loop one turn. */
  struct timespec delay = {ms / 1000u, (long)(ms % 1000u) * 1000000L};
  if (ms == 0u)
    delay.tv_nsec = 1000000L;
  while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
  }
  h2_web_state_resume(depth);
}

void h2_web_state_wait(uint32_t ms) {
  if (state_depth == 0 || emscripten_is_main_runtime_thread())
    abort();
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_nsec += (long)ms * 1000000L;
  until.tv_sec += until.tv_nsec / 1000000000L;
  until.tv_nsec %= 1000000000L;
  pthread_cond_timedwait(&state_changed, &state_mutex, &until);
}
void h2_web_state_notify(void) { pthread_cond_broadcast(&state_changed); }
