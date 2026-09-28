#ifndef H2_WEB_MAIN_THREAD_H
#define H2_WEB_MAIN_THREAD_H

#include <emscripten.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Scalar return storage shared with a browser callback. */
typedef union h2_web_main_result {
  int32_t i32;
  uint32_t u32;
  int64_t i64;
  uint64_t u64;
  double f64;
  void *ptr;
} h2_web_main_result_t;

/** Opaque completion token, valid until h2_web_main_complete(). */
typedef struct h2_web_main_completion h2_web_main_completion_t;

/** Run callback(context, result, completion) on the browser main thread.
 * Wait until completion, then return result by value. The caller owns context
 * until return. Ordinary C callbacks may use any context type; EM_JS callbacks
 * use an array of pointers to typed C argument values, read with getValue().
 *
 * The callback must complete exactly once, either inline or after its Promise
 * settles, and must not access context/result after completion. An existing UI
 * caller must complete inline. Worker callers release the browser-state guard
 * while waiting. This entry has no per-operation adapters or argument limit. */
h2_web_main_result_t
h2_web_main_call(void (*callback)(void *context, h2_web_main_result_t *result,
                                  h2_web_main_completion_t *completion),
                 void *context);
void h2_web_main_complete(h2_web_main_completion_t *completion);

/** Sleep the calling Worker while keeping browser UI events independent. */
void h2_web_worker_sleep(uint32_t milliseconds);
void h2_web_state_enter(void);
void h2_web_state_leave(int *guard);
unsigned h2_web_state_pause(void);
void h2_web_state_resume(unsigned depth);
/** Called while holding a state guard; atomically releases it while waiting. */
void h2_web_state_wait(uint32_t milliseconds);
void h2_web_state_notify(void);
#define H2_WEB_STATE_GUARD()                                                   \
  int h2_web_guard __attribute__((cleanup(h2_web_state_leave))) = 0;           \
  h2_web_state_enter()
#ifdef __cplusplus
}
#endif

#endif
