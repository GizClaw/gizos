#define LIBCO_C
#include "libco.h"

#include <emscripten/fiber.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LIBCO_EMSCRIPTEN_ALIGNMENT 16u
#define H2_LIBCO_EMSCRIPTEN_ASYNCIFY_STACK_SIZE (64u * 1024u)
#define H2_LIBCO_EMSCRIPTEN_PARTITION_GUARD UINT64_C(0x68326173796e6321)

typedef struct h2_libco_emscripten_thread {
  emscripten_fiber_t fiber;
  void (*entry)(void);
} h2_libco_emscripten_thread_t;

static h2_libco_emscripten_thread_t h2_libco_emscripten_primary;
static h2_libco_emscripten_thread_t *h2_libco_emscripten_running;
static unsigned char h2_libco_emscripten_primary_asyncify[
    H2_LIBCO_EMSCRIPTEN_ASYNCIFY_STACK_SIZE];

static size_t h2_libco_emscripten_align(size_t value) {
  return (value + H2_LIBCO_EMSCRIPTEN_ALIGNMENT - 1u) &
         ~(H2_LIBCO_EMSCRIPTEN_ALIGNMENT - 1u);
}

size_t h2_libco_emscripten_context_overhead(void) {
  return h2_libco_emscripten_align(sizeof(h2_libco_emscripten_thread_t)) +
         H2_LIBCO_EMSCRIPTEN_ASYNCIFY_STACK_SIZE +
         H2_LIBCO_EMSCRIPTEN_ALIGNMENT;
}

int h2_libco_emscripten_context_valid(const void *memory, size_t size) {
  const size_t metadata_size =
      h2_libco_emscripten_align(sizeof(h2_libco_emscripten_thread_t));
  uint64_t guard = 0u;
  if (memory == NULL || size <= h2_libco_emscripten_context_overhead()) {
    return 0;
  }
  memcpy(&guard, (const unsigned char *)memory + metadata_size +
                     H2_LIBCO_EMSCRIPTEN_ASYNCIFY_STACK_SIZE,
         sizeof(guard));
  return guard == H2_LIBCO_EMSCRIPTEN_PARTITION_GUARD;
}

static void h2_libco_emscripten_thunk(void *user) {
  h2_libco_emscripten_thread_t *thread = user;
  thread->entry();
  abort();
}

cothread_t co_active(void) {
  if (h2_libco_emscripten_running == NULL) {
    h2_libco_emscripten_running = &h2_libco_emscripten_primary;
    emscripten_fiber_init_from_current_context(
        &h2_libco_emscripten_primary.fiber,
        h2_libco_emscripten_primary_asyncify,
        sizeof(h2_libco_emscripten_primary_asyncify));
  }
  return (cothread_t)h2_libco_emscripten_running;
}

cothread_t co_derive(void *memory, unsigned int size,
                     void (*entry)(void)) {
  const size_t metadata_size =
      h2_libco_emscripten_align(sizeof(h2_libco_emscripten_thread_t));
  const size_t overhead = h2_libco_emscripten_context_overhead();
  if (memory == NULL || entry == NULL || size <= overhead) {
    return (cothread_t)0;
  }
  (void)co_active();
  h2_libco_emscripten_thread_t *thread = memory;
  unsigned char *asyncify_stack = (unsigned char *)memory + metadata_size;
  unsigned char *partition = asyncify_stack +
                             H2_LIBCO_EMSCRIPTEN_ASYNCIFY_STACK_SIZE;
  const uint64_t guard = H2_LIBCO_EMSCRIPTEN_PARTITION_GUARD;
  memcpy(partition, &guard, sizeof(guard));
  unsigned char *c_stack = partition + H2_LIBCO_EMSCRIPTEN_ALIGNMENT;
  thread->entry = entry;
  emscripten_fiber_init(&thread->fiber, h2_libco_emscripten_thunk, thread,
                        c_stack, (size_t)size - overhead, asyncify_stack,
                        H2_LIBCO_EMSCRIPTEN_ASYNCIFY_STACK_SIZE);
  return (cothread_t)thread;
}

cothread_t co_create(unsigned int size, void (*entry)(void)) {
  const size_t overhead = h2_libco_emscripten_context_overhead();
  if ((size_t)size > SIZE_MAX - overhead ||
      (size_t)size + overhead > UINT_MAX) {
    return (cothread_t)0;
  }
  void *memory = malloc((size_t)size + overhead);
  if (memory == NULL) {
    return (cothread_t)0;
  }
  cothread_t thread = co_derive(memory, (unsigned int)((size_t)size + overhead),
                                entry);
  if (thread == NULL) {
    free(memory);
  }
  return thread;
}

void co_delete(cothread_t thread) {
  free(thread);
}

void co_switch(cothread_t thread) {
  h2_libco_emscripten_thread_t *next = thread;
  h2_libco_emscripten_thread_t *previous = h2_libco_emscripten_running;
  if (next == NULL || previous == NULL || next == previous) {
    return;
  }
  h2_libco_emscripten_running = next;
  emscripten_fiber_swap(&previous->fiber, &next->fiber);
}

int co_serializable(void) {
  return 0;
}

#ifdef __cplusplus
}
#endif
