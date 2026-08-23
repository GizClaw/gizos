#define _POSIX_C_SOURCE 200809L

#include "h2_linux_platform.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct h2_pal_mutex {
  pthread_mutex_t native;
  const h2_pal_mem_api_t *allocator;
  int recursive;
};

struct h2_pal_semaphore {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  const h2_pal_mem_api_t *allocator;
  uint32_t count;
  uint32_t max_count;
};

struct h2_pal_cond {
  pthread_cond_t native;
  const h2_pal_mem_api_t *allocator;
};

static void *sync_alloc(const h2_pal_mem_api_t *allocator, size_t size) {
  return allocator != NULL ? h2_pal_mem_alloc(allocator, size) : malloc(size);
}

static void sync_free(const h2_pal_mem_api_t *allocator, void *memory) {
  if (allocator != NULL) {
    h2_pal_mem_free(allocator, memory);
  } else {
    free(memory);
  }
}

static h2_pal_result_t sync_deadline(uint32_t timeout_ms,
                                     struct timespec *out_deadline) {
  if (clock_gettime(CLOCK_MONOTONIC, out_deadline) != 0) {
    return H2_PAL_ERR_IO;
  }
  out_deadline->tv_sec += (time_t)(timeout_ms / 1000u);
  out_deadline->tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
  if (out_deadline->tv_nsec >= 1000000000L) {
    ++out_deadline->tv_sec;
    out_deadline->tv_nsec -= 1000000000L;
  }
  return H2_PAL_OK;
}

static int sync_condition_init(pthread_cond_t *condition) {
  pthread_condattr_t attributes;
  if (pthread_condattr_init(&attributes) != 0) {
    return -1;
  }
  int result = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
  if (result == 0) {
    result = pthread_cond_init(condition, &attributes);
  }
  (void)pthread_condattr_destroy(&attributes);
  return result;
}

static h2_pal_result_t
linux_sync_create_mutex(void *user, const h2_pal_mutex_config_t *config,
                        h2_pal_mutex_t **out_mutex) {
  (void)user;
  if (config == NULL || out_mutex == NULL ||
      (config->flags & ~H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_mutex = NULL;
  h2_pal_mutex_t *mutex = sync_alloc(config->allocator, sizeof(*mutex));
  if (mutex == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(mutex, 0, sizeof(*mutex));
  mutex->allocator = config->allocator;
  mutex->recursive = (config->flags & H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u;

  pthread_mutexattr_t attributes;
  if (pthread_mutexattr_init(&attributes) != 0) {
    sync_free(config->allocator, mutex);
    return H2_PAL_ERR_IO;
  }
  int result = 0;
  if (mutex->recursive != 0) {
    result = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
  }
  if (result == 0) {
    result = pthread_mutex_init(&mutex->native, &attributes);
  }
  (void)pthread_mutexattr_destroy(&attributes);
  if (result != 0) {
    sync_free(config->allocator, mutex);
    return H2_PAL_ERR_IO;
  }
  *out_mutex = mutex;
  return H2_PAL_OK;
}

static h2_pal_result_t linux_sync_destroy_mutex(void *user,
                                                h2_pal_mutex_t *mutex) {
  (void)user;
  if (mutex == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const int result = pthread_mutex_destroy(&mutex->native);
  if (result != 0) {
    return result == EBUSY ? H2_PAL_ERR_BUSY : H2_PAL_ERR_IO;
  }
  const h2_pal_mem_api_t *allocator = mutex->allocator;
  sync_free(allocator, mutex);
  return H2_PAL_OK;
}

static h2_pal_result_t linux_sync_lock_mutex(void *user,
                                             h2_pal_mutex_t *mutex) {
  (void)user;
  return pthread_mutex_lock(&mutex->native) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t linux_sync_try_lock_mutex(void *user,
                                                 h2_pal_mutex_t *mutex) {
  (void)user;
  const int result = pthread_mutex_trylock(&mutex->native);
  if (result == 0) {
    return H2_PAL_OK;
  }
  return result == EBUSY ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO;
}

static h2_pal_result_t linux_sync_unlock_mutex(void *user,
                                               h2_pal_mutex_t *mutex) {
  (void)user;
  return pthread_mutex_unlock(&mutex->native) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t
linux_sync_create_semaphore(void *user, const h2_pal_semaphore_config_t *config,
                            h2_pal_semaphore_t **out_semaphore) {
  (void)user;
  if (config == NULL || out_semaphore == NULL || config->max_count == 0u ||
      config->initial_count > config->max_count) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_semaphore = NULL;
  h2_pal_semaphore_t *semaphore =
      sync_alloc(config->allocator, sizeof(*semaphore));
  if (semaphore == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(semaphore, 0, sizeof(*semaphore));
  semaphore->allocator = config->allocator;
  semaphore->count = config->initial_count;
  semaphore->max_count = config->max_count;
  if (pthread_mutex_init(&semaphore->mutex, NULL) != 0) {
    sync_free(config->allocator, semaphore);
    return H2_PAL_ERR_IO;
  }
  if (sync_condition_init(&semaphore->condition) != 0) {
    (void)pthread_mutex_destroy(&semaphore->mutex);
    sync_free(config->allocator, semaphore);
    return H2_PAL_ERR_IO;
  }
  *out_semaphore = semaphore;
  return H2_PAL_OK;
}

static h2_pal_result_t
linux_sync_destroy_semaphore(void *user, h2_pal_semaphore_t *semaphore) {
  (void)user;
  if (semaphore == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const int lock_result = pthread_mutex_trylock(&semaphore->mutex);
  if (lock_result != 0) {
    return lock_result == EBUSY ? H2_PAL_ERR_BUSY : H2_PAL_ERR_IO;
  }
  const int condition_result = pthread_cond_destroy(&semaphore->condition);
  const int unlock_result = pthread_mutex_unlock(&semaphore->mutex);
  if (condition_result != 0) {
    return condition_result == EBUSY ? H2_PAL_ERR_BUSY : H2_PAL_ERR_IO;
  }
  if (unlock_result != 0) {
    return H2_PAL_ERR_IO;
  }
  const int mutex_result = pthread_mutex_destroy(&semaphore->mutex);
  if (mutex_result != 0) {
    return mutex_result == EBUSY ? H2_PAL_ERR_BUSY : H2_PAL_ERR_IO;
  }
  const h2_pal_mem_api_t *allocator = semaphore->allocator;
  sync_free(allocator, semaphore);
  return H2_PAL_OK;
}

static h2_pal_result_t linux_sync_take_semaphore(void *user,
                                                 h2_pal_semaphore_t *semaphore,
                                                 uint32_t timeout_ms) {
  (void)user;
  if (pthread_mutex_lock(&semaphore->mutex) != 0) {
    return H2_PAL_ERR_IO;
  }
  h2_pal_result_t result = H2_PAL_OK;
  struct timespec deadline = {0};
  if (timeout_ms != H2_PAL_SYNC_NO_WAIT &&
      timeout_ms != H2_PAL_SYNC_WAIT_FOREVER) {
    result = sync_deadline(timeout_ms, &deadline);
  }
  while (result == H2_PAL_OK && semaphore->count == 0u) {
    if (timeout_ms == H2_PAL_SYNC_NO_WAIT) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    const int wait_result =
        timeout_ms == H2_PAL_SYNC_WAIT_FOREVER
            ? pthread_cond_wait(&semaphore->condition, &semaphore->mutex)
            : pthread_cond_timedwait(&semaphore->condition, &semaphore->mutex,
                                     &deadline);
    if (wait_result == ETIMEDOUT) {
      result = H2_PAL_ERR_TIMEOUT;
    } else if (wait_result != 0) {
      result = H2_PAL_ERR_IO;
    }
  }
  if (result == H2_PAL_OK) {
    --semaphore->count;
  }
  if (pthread_mutex_unlock(&semaphore->mutex) != 0 && result == H2_PAL_OK) {
    result = H2_PAL_ERR_IO;
  }
  return result;
}

static h2_pal_result_t
linux_sync_give_semaphore(void *user, h2_pal_semaphore_t *semaphore) {
  (void)user;
  if (pthread_mutex_lock(&semaphore->mutex) != 0) {
    return H2_PAL_ERR_IO;
  }
  h2_pal_result_t result = H2_PAL_OK;
  if (semaphore->count == semaphore->max_count) {
    result = H2_PAL_ERR_FULL;
  } else {
    ++semaphore->count;
    if (pthread_cond_signal(&semaphore->condition) != 0) {
      result = H2_PAL_ERR_IO;
    }
  }
  if (pthread_mutex_unlock(&semaphore->mutex) != 0 && result == H2_PAL_OK) {
    result = H2_PAL_ERR_IO;
  }
  return result;
}

static h2_pal_result_t
linux_sync_create_cond(void *user, const h2_pal_cond_config_t *config,
                       h2_pal_cond_t **out_cond) {
  (void)user;
  if (config == NULL || out_cond == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_cond = NULL;
  h2_pal_cond_t *condition = sync_alloc(config->allocator, sizeof(*condition));
  if (condition == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(condition, 0, sizeof(*condition));
  condition->allocator = config->allocator;
  if (sync_condition_init(&condition->native) != 0) {
    sync_free(config->allocator, condition);
    return H2_PAL_ERR_IO;
  }
  *out_cond = condition;
  return H2_PAL_OK;
}

static h2_pal_result_t linux_sync_destroy_cond(void *user,
                                               h2_pal_cond_t *condition) {
  (void)user;
  if (condition == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const int result = pthread_cond_destroy(&condition->native);
  if (result != 0) {
    return result == EBUSY ? H2_PAL_ERR_BUSY : H2_PAL_ERR_IO;
  }
  const h2_pal_mem_api_t *allocator = condition->allocator;
  sync_free(allocator, condition);
  return H2_PAL_OK;
}

static h2_pal_result_t linux_sync_wait_cond(void *user,
                                            h2_pal_cond_t *condition,
                                            h2_pal_mutex_t *mutex,
                                            uint32_t timeout_ms) {
  (void)user;
  if (mutex->recursive != 0) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  if (timeout_ms == H2_PAL_SYNC_NO_WAIT) {
    return H2_PAL_ERR_TIMEOUT;
  }
  int result;
  if (timeout_ms == H2_PAL_SYNC_WAIT_FOREVER) {
    result = pthread_cond_wait(&condition->native, &mutex->native);
  } else {
    struct timespec deadline;
    h2_pal_result_t deadline_result = sync_deadline(timeout_ms, &deadline);
    if (deadline_result != H2_PAL_OK) {
      return deadline_result;
    }
    result =
        pthread_cond_timedwait(&condition->native, &mutex->native, &deadline);
  }
  if (result == 0) {
    return H2_PAL_OK;
  }
  return result == ETIMEDOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
}

static h2_pal_result_t linux_sync_signal_cond(void *user,
                                              h2_pal_cond_t *condition) {
  (void)user;
  return pthread_cond_signal(&condition->native) == 0 ? H2_PAL_OK
                                                      : H2_PAL_ERR_IO;
}

static h2_pal_result_t linux_sync_broadcast_cond(void *user,
                                                 h2_pal_cond_t *condition) {
  (void)user;
  return pthread_cond_broadcast(&condition->native) == 0 ? H2_PAL_OK
                                                         : H2_PAL_ERR_IO;
}

static const h2_pal_sync_vtable_t s_linux_sync_vtable = {
    .create_mutex = linux_sync_create_mutex,
    .destroy_mutex = linux_sync_destroy_mutex,
    .lock_mutex = linux_sync_lock_mutex,
    .try_lock_mutex = linux_sync_try_lock_mutex,
    .unlock_mutex = linux_sync_unlock_mutex,
    .create_semaphore = linux_sync_create_semaphore,
    .destroy_semaphore = linux_sync_destroy_semaphore,
    .take_semaphore = linux_sync_take_semaphore,
    .give_semaphore = linux_sync_give_semaphore,
    .create_cond = linux_sync_create_cond,
    .destroy_cond = linux_sync_destroy_cond,
    .wait_cond = linux_sync_wait_cond,
    .signal_cond = linux_sync_signal_cond,
    .broadcast_cond = linux_sync_broadcast_cond,
};

static const h2_pal_sync_api_t s_linux_sync_api = {
    .user = NULL,
    .vtable = &s_linux_sync_vtable,
};

const h2_pal_sync_api_t *h2_linux_sync_api(void) { return &s_linux_sync_api; }
