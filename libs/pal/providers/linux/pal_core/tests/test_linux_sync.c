#define _POSIX_C_SOURCE 200809L

#include "h2_linux_platform.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

typedef struct condition_signal_context {
  const h2_pal_sync_api_t *sync;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *condition;
} condition_signal_context_t;

static void *signal_condition(void *user) {
  condition_signal_context_t *context = user;
  const struct timespec delay = {.tv_nsec = 10000000L};
  (void)nanosleep(&delay, NULL);
  assert(h2_pal_mutex_lock(context->sync, context->mutex) == H2_PAL_OK);
  assert(h2_pal_cond_signal(context->sync, context->condition) == H2_PAL_OK);
  assert(h2_pal_mutex_unlock(context->sync, context->mutex) == H2_PAL_OK);
  return NULL;
}

static void test_mutex(const h2_pal_sync_api_t *sync) {
  h2_pal_mutex_t *mutex = NULL;
  const h2_pal_mutex_config_t config = {
      .name = "linux-test-mutex",
      .flags = H2_PAL_MUTEX_FLAG_NONE,
  };
  assert(h2_pal_mutex_create(sync, &config, &mutex) == H2_PAL_OK);
  assert(h2_pal_mutex_lock(sync, mutex) == H2_PAL_OK);
  assert(h2_pal_mutex_try_lock(sync, mutex) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_pal_mutex_unlock(sync, mutex) == H2_PAL_OK);
  assert(h2_pal_mutex_destroy(sync, mutex) == H2_PAL_OK);

  const h2_pal_mutex_config_t recursive_config = {
      .name = "linux-test-recursive-mutex",
      .flags = H2_PAL_MUTEX_FLAG_RECURSIVE,
  };
  mutex = NULL;
  assert(h2_pal_mutex_create(sync, &recursive_config, &mutex) == H2_PAL_OK);
  assert(h2_pal_mutex_lock(sync, mutex) == H2_PAL_OK);
  assert(h2_pal_mutex_lock(sync, mutex) == H2_PAL_OK);
  assert(h2_pal_mutex_unlock(sync, mutex) == H2_PAL_OK);
  assert(h2_pal_mutex_unlock(sync, mutex) == H2_PAL_OK);
  assert(h2_pal_mutex_destroy(sync, mutex) == H2_PAL_OK);
}

static void test_semaphore(const h2_pal_sync_api_t *sync) {
  h2_pal_semaphore_t *semaphore = NULL;
  const h2_pal_semaphore_config_t config = {
      .name = "linux-test-semaphore",
      .initial_count = 0u,
      .max_count = 1u,
  };
  assert(h2_pal_semaphore_create(sync, &config, &semaphore) == H2_PAL_OK);
  assert(h2_pal_semaphore_take(sync, semaphore, H2_PAL_SYNC_NO_WAIT) ==
         H2_PAL_ERR_TIMEOUT);
  assert(h2_pal_semaphore_take(sync, semaphore, 1u) == H2_PAL_ERR_TIMEOUT);
  assert(h2_pal_semaphore_give(sync, semaphore) == H2_PAL_OK);
  assert(h2_pal_semaphore_give(sync, semaphore) == H2_PAL_ERR_FULL);
  assert(h2_pal_semaphore_take(sync, semaphore, 10u) == H2_PAL_OK);
  assert(h2_pal_semaphore_destroy(sync, semaphore) == H2_PAL_OK);
}

static void test_condition(const h2_pal_sync_api_t *sync) {
  h2_pal_mutex_t *mutex = NULL;
  h2_pal_cond_t *condition = NULL;
  const h2_pal_mutex_config_t mutex_config = {
      .name = "linux-test-condition-mutex",
      .flags = H2_PAL_MUTEX_FLAG_NONE,
  };
  const h2_pal_cond_config_t condition_config = {
      .name = "linux-test-condition",
  };
  assert(h2_pal_mutex_create(sync, &mutex_config, &mutex) == H2_PAL_OK);
  assert(h2_pal_cond_create(sync, &condition_config, &condition) == H2_PAL_OK);
  assert(h2_pal_mutex_lock(sync, mutex) == H2_PAL_OK);

  condition_signal_context_t context = {
      .sync = sync,
      .mutex = mutex,
      .condition = condition,
  };
  pthread_t thread;
  assert(pthread_create(&thread, NULL, signal_condition, &context) == 0);
  assert(h2_pal_cond_wait(sync, condition, mutex, 1000u) == H2_PAL_OK);
  assert(h2_pal_mutex_unlock(sync, mutex) == H2_PAL_OK);
  assert(pthread_join(thread, NULL) == 0);
  assert(h2_pal_mutex_lock(sync, mutex) == H2_PAL_OK);
  assert(h2_pal_cond_wait(sync, condition, mutex, 1u) == H2_PAL_ERR_TIMEOUT);
  assert(h2_pal_mutex_unlock(sync, mutex) == H2_PAL_OK);
  assert(h2_pal_cond_destroy(sync, condition) == H2_PAL_OK);
  assert(h2_pal_mutex_destroy(sync, mutex) == H2_PAL_OK);
}

int main(void) {
  const h2_pal_sync_api_t *sync = h2_linux_sync_api();
  test_mutex(sync);
  test_semaphore(sync);
  test_condition(sync);
  puts("linux sync tests passed");
  return 0;
}
