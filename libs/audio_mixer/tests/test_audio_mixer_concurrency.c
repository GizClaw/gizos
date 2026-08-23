#include "h2_audio_mixer.h"
#include "h2_desktop_platform.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_QUEUE_COUNT 8u
#define TEST_QUEUE_ITEMS 8u
#define TEST_QUEUE_ITEM_BYTES 64u

typedef struct test_env test_env_t;

typedef struct test_queue {
  test_env_t *env;
  size_t item_size;
  size_t capacity;
  size_t head;
  size_t count;
  uint8_t items[TEST_QUEUE_ITEMS][TEST_QUEUE_ITEM_BYTES];
  int used;
  int closed;
  int drain;
} test_queue_t;

struct test_env {
  h2_audio_mixer_t mixer;
  h2_pal_queue_api_t queue_api;
  h2_pal_sync_api_t sync_api;
  const h2_pal_sync_api_t *base_sync;
  test_queue_t queues[TEST_QUEUE_COUNT];
  pthread_mutex_t control_mutex;
  pthread_cond_t control_cond;
  int fail_mutex_create;
  int fail_cond_create;
  int fail_cond_broadcast;
  pthread_t watched_thread;
  int watch_thread_wait;
  int watched_wait_entered;
  int fail_queue_close;
  int mutex_destroy_count;
  int queue_close_count;
  int watch_contended_lock;
  int contended_lock_entered;
  test_queue_t *pause_recv_queue;
  int recv_entered;
  int release_recv;
  test_queue_t *pause_send_queue;
  int send_entered;
  int release_send;
};

static void control_lock(test_env_t *env) {
  assert(pthread_mutex_lock(&env->control_mutex) == 0);
}

static void control_unlock(test_env_t *env) {
  assert(pthread_mutex_unlock(&env->control_mutex) == 0);
}

static void control_broadcast(test_env_t *env) {
  assert(pthread_cond_broadcast(&env->control_cond) == 0);
}

static void control_wait(test_env_t *env) {
  assert(pthread_cond_wait(&env->control_cond, &env->control_mutex) == 0);
}

static h2_pal_result_t sync_mutex_create(void *user,
                                         const h2_pal_mutex_config_t *config,
                                         h2_pal_mutex_t **out_mutex) {
  test_env_t *env = user;
  if (env->fail_mutex_create) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  return h2_pal_mutex_create(env->base_sync, config, out_mutex);
}

static h2_pal_result_t sync_mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
  test_env_t *env = user;
  ++env->mutex_destroy_count;
  return h2_pal_mutex_destroy(env->base_sync, mutex);
}

static h2_pal_result_t sync_mutex_lock(void *user, h2_pal_mutex_t *mutex) {
  test_env_t *env = user;
  control_lock(env);
  if (env->watch_contended_lock && env->recv_entered && !env->release_recv) {
    env->contended_lock_entered = 1;
    control_broadcast(env);
  }
  control_unlock(env);
  return h2_pal_mutex_lock(env->base_sync, mutex);
}

static h2_pal_result_t sync_mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
  test_env_t *env = user;
  return h2_pal_mutex_unlock(env->base_sync, mutex);
}

static h2_pal_result_t sync_cond_create(void *user,
                                        const h2_pal_cond_config_t *config,
                                        h2_pal_cond_t **out_cond) {
  test_env_t *env = user;
  if (env->fail_cond_create) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  return h2_pal_cond_create(env->base_sync, config, out_cond);
}

static h2_pal_result_t sync_cond_destroy(void *user, h2_pal_cond_t *cond) {
  test_env_t *env = user;
  return h2_pal_cond_destroy(env->base_sync, cond);
}

static h2_pal_result_t sync_cond_wait(void *user, h2_pal_cond_t *cond,
                                      h2_pal_mutex_t *mutex,
                                      uint32_t timeout_ms) {
  test_env_t *env = user;
  control_lock(env);
  if (env->watch_thread_wait &&
      pthread_equal(pthread_self(), env->watched_thread)) {
    env->watched_wait_entered = 1;
    control_broadcast(env);
  }
  control_unlock(env);
  return h2_pal_cond_wait(env->base_sync, cond, mutex, timeout_ms);
}

static h2_pal_result_t sync_cond_broadcast(void *user, h2_pal_cond_t *cond) {
  test_env_t *env = user;
  if (env->fail_cond_broadcast > 0) {
    --env->fail_cond_broadcast;
    return H2_PAL_ERR_IO;
  }
  return h2_pal_cond_broadcast(env->base_sync, cond);
}

static const h2_pal_sync_vtable_t TEST_SYNC_VTABLE = {
    .create_mutex = sync_mutex_create,
    .destroy_mutex = sync_mutex_destroy,
    .lock_mutex = sync_mutex_lock,
    .unlock_mutex = sync_mutex_unlock,
    .create_cond = sync_cond_create,
    .destroy_cond = sync_cond_destroy,
    .wait_cond = sync_cond_wait,
    .broadcast_cond = sync_cond_broadcast,
};

static int queue_create(void *user, const h2_pal_queue_config_t *config,
                        h2_pal_queue_t **out_queue) {
  test_env_t *env = user;
  if (config == NULL || out_queue == NULL || config->item_size == 0u ||
      config->item_size > TEST_QUEUE_ITEM_BYTES || config->item_count == 0u ||
      config->item_count > TEST_QUEUE_ITEMS) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  control_lock(env);
  for (size_t index = 0u; index < TEST_QUEUE_COUNT; ++index) {
    test_queue_t *queue = &env->queues[index];
    if (queue->used) {
      continue;
    }
    memset(queue, 0, sizeof(*queue));
    queue->env = env;
    queue->used = 1;
    queue->item_size = config->item_size;
    queue->capacity = config->item_count;
    queue->drain = config->item_size == sizeof(uint64_t);
    *out_queue = (h2_pal_queue_t *)queue;
    control_unlock(env);
    return H2_PAL_QUEUE_OK;
  }
  control_unlock(env);
  return H2_PAL_QUEUE_ERR_NO_MEMORY;
}

static void queue_destroy(void *user, h2_pal_queue_t *opaque) {
  test_env_t *env = user;
  control_lock(env);
  memset((test_queue_t *)opaque, 0, sizeof(test_queue_t));
  control_broadcast(env);
  control_unlock(env);
}

static int queue_send(void *user, h2_pal_queue_t *opaque, const void *item,
                      uint32_t timeout_ms) {
  (void)timeout_ms;
  test_env_t *env = user;
  test_queue_t *queue = (test_queue_t *)opaque;
  if (queue == NULL || item == NULL) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  control_lock(env);
  if (queue == env->pause_send_queue) {
    env->send_entered = 1;
    control_broadcast(env);
    while (!env->release_send) {
      control_wait(env);
    }
  }
  if (!queue->used || queue->closed) {
    control_unlock(env);
    return H2_PAL_QUEUE_ERR_CLOSED;
  }
  if (queue->count == queue->capacity) {
    control_unlock(env);
    return H2_PAL_QUEUE_ERR_TIMEOUT;
  }
  const size_t tail = (queue->head + queue->count) % queue->capacity;
  memcpy(queue->items[tail], item, queue->item_size);
  ++queue->count;
  control_unlock(env);
  return H2_PAL_QUEUE_OK;
}

static int queue_send_latest(void *user, h2_pal_queue_t *opaque,
                             const void *item) {
  test_env_t *env = user;
  test_queue_t *queue = (test_queue_t *)opaque;
  control_lock(env);
  if (queue != NULL && queue->count == queue->capacity) {
    queue->head = (queue->head + 1u) % queue->capacity;
    --queue->count;
  }
  control_unlock(env);
  return queue_send(user, opaque, item, H2_PAL_QUEUE_NO_WAIT);
}

static int queue_recv(void *user, h2_pal_queue_t *opaque, void *out_item,
                      uint32_t timeout_ms) {
  (void)timeout_ms;
  test_env_t *env = user;
  test_queue_t *queue = (test_queue_t *)opaque;
  if (queue == NULL || out_item == NULL) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  control_lock(env);
  if (queue == env->pause_recv_queue) {
    env->recv_entered = 1;
    control_broadcast(env);
    while (!env->release_recv) {
      control_wait(env);
    }
  }
  if (queue->used && queue->count > 0u) {
    memcpy(out_item, queue->items[queue->head], queue->item_size);
    queue->head = (queue->head + 1u) % queue->capacity;
    --queue->count;
    control_unlock(env);
    return H2_PAL_QUEUE_OK;
  }
  if (!queue->used || queue->closed) {
    control_unlock(env);
    return H2_PAL_QUEUE_ERR_CLOSED;
  }
  control_unlock(env);
  return H2_PAL_QUEUE_ERR_TIMEOUT;
}

static int queue_reset(void *user, h2_pal_queue_t *opaque) {
  test_env_t *env = user;
  test_queue_t *queue = (test_queue_t *)opaque;
  control_lock(env);
  queue->head = 0u;
  queue->count = 0u;
  control_unlock(env);
  return H2_PAL_QUEUE_OK;
}

static int queue_close(void *user, h2_pal_queue_t *opaque) {
  test_env_t *env = user;
  test_queue_t *queue = (test_queue_t *)opaque;
  control_lock(env);
  ++env->queue_close_count;
  if (env->fail_queue_close > 0) {
    --env->fail_queue_close;
    control_broadcast(env);
    control_unlock(env);
    return H2_PAL_QUEUE_ERR_IO;
  }
  queue->closed = 1;
  control_broadcast(env);
  control_unlock(env);
  return H2_PAL_QUEUE_OK;
}

static const h2_pal_queue_vtable_t TEST_QUEUE_VTABLE = {
    .create = queue_create,
    .destroy = queue_destroy,
    .send = queue_send,
    .send_latest = queue_send_latest,
    .recv = queue_recv,
    .reset = queue_reset,
    .close = queue_close,
};

static const h2_audio_pcm_format_t TEST_FORMAT = {
    .sample_rate_hz = 16000u,
    .frame_samples_per_channel = 4u,
    .channels = 1u,
    .sample_format = H2_AUDIO_SAMPLE_S16LE,
};

static const h2_audio_track_config_t TEST_TRACK_CONFIG = {
    .name = "audio-mixer-concurrency-test",
    .format =
        {
            .sample_rate_hz = 16000u,
            .frame_samples_per_channel = 4u,
            .channels = 1u,
            .sample_format = H2_AUDIO_SAMPLE_S16LE,
        },
    .volume_factor_milli = 1000u,
    .buffer_frames = 4u,
};

static void env_init(test_env_t *env) {
  memset(env, 0, sizeof(*env));
  assert(pthread_mutex_init(&env->control_mutex, NULL) == 0);
  assert(pthread_cond_init(&env->control_cond, NULL) == 0);
  env->base_sync = h2_desktop_platform_sync_api();
  env->queue_api.user = env;
  env->queue_api.vtable = &TEST_QUEUE_VTABLE;
  env->sync_api.user = env;
  env->sync_api.vtable = &TEST_SYNC_VTABLE;
}

static h2_audio_mixer_config_t mixer_config(test_env_t *env,
                                            uint8_t max_tracks) {
  const h2_audio_mixer_config_t config = {
      .format = TEST_FORMAT,
      .max_tracks = max_tracks,
      .track_queue_frames = 4u,
      .master_factor_milli = 1000u,
      .queue_api = &env->queue_api,
      .sync_api = &env->sync_api,
  };
  return config;
}

static void env_deinit(test_env_t *env) {
  h2_audio_mixer_deinit(&env->mixer);
  assert(pthread_cond_destroy(&env->control_cond) == 0);
  assert(pthread_mutex_destroy(&env->control_mutex) == 0);
}

static h2_pal_audio_track_t *create_track(test_env_t *env) {
  h2_pal_audio_track_t *track = NULL;
  assert(h2_audio_mixer_create_track(&env->mixer, NULL, &TEST_TRACK_CONFIG,
                                     &track) == H2_AUDIO_OK);
  return track;
}

static h2_audio_frame_t frame_for(int16_t samples[4]) {
  h2_audio_frame_t frame =
      h2_audio_frame_for_buffer(samples, sizeof(int16_t) * 4u, TEST_FORMAT);
  frame.bytes = sizeof(int16_t) * 4u;
  return frame;
}

static void test_init_and_close_failures(void) {
  test_env_t env;
  env_init(&env);
  h2_audio_mixer_config_t config = mixer_config(&env, 1u);
  config.sync_api = NULL;
  assert(h2_audio_mixer_init(&env.mixer, &config) == H2_AUDIO_ERR_INVALID_ARG);
  env_deinit(&env);

  env_init(&env);
  config = mixer_config(&env, 1u);
  env.fail_mutex_create = 1;
  assert(h2_audio_mixer_init(&env.mixer, &config) == H2_AUDIO_ERR_NO_MEMORY);
  assert(env.mixer.impl == NULL);
  env_deinit(&env);

  env_init(&env);
  config = mixer_config(&env, 1u);
  env.fail_cond_create = 1;
  assert(h2_audio_mixer_init(&env.mixer, &config) == H2_AUDIO_ERR_NO_MEMORY);
  assert(env.mutex_destroy_count == 1);
  assert(env.mixer.impl == NULL);
  env_deinit(&env);

  env_init(&env);
  config = mixer_config(&env, 1u);
  assert(h2_audio_mixer_init(&env.mixer, &config) == H2_AUDIO_OK);
  h2_pal_audio_track_t *track = create_track(&env);
  env.fail_queue_close = 1;
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_ERR_IO);
  h2_pal_audio_track_t *replacement = NULL;
  assert(h2_audio_mixer_create_track(&env.mixer, NULL, &TEST_TRACK_CONFIG,
                                     &replacement) == H2_AUDIO_ERR_UNAVAILABLE);
  assert(h2_audio_mixer_close_all(&env.mixer) == H2_AUDIO_OK);
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);
  replacement = create_track(&env);
  assert(h2_pal_audio_track_close(replacement) == H2_AUDIO_OK);
  env_deinit(&env);

  env_init(&env);
  config = mixer_config(&env, 1u);
  assert(h2_audio_mixer_init(&env.mixer, &config) == H2_AUDIO_OK);
  track = create_track(&env);
  env.fail_cond_broadcast = 1;
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_ERR_IO);
  replacement = NULL;
  assert(h2_audio_mixer_create_track(&env.mixer, NULL, &TEST_TRACK_CONFIG,
                                     &replacement) == H2_AUDIO_ERR_UNAVAILABLE);
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);
  replacement = create_track(&env);
  assert(h2_pal_audio_track_close(replacement) == H2_AUDIO_OK);
  env_deinit(&env);
}

typedef struct read_context {
  test_env_t *env;
  int16_t samples[4];
  int result;
} read_context_t;

static void *read_thread_main(void *opaque) {
  read_context_t *context = opaque;
  h2_audio_frame_t frame = frame_for(context->samples);
  context->result = h2_audio_mixer_read(&context->env->mixer, &frame);
  return NULL;
}

typedef struct replace_context {
  test_env_t *env;
  h2_pal_audio_track_t *old_track;
  h2_pal_audio_track_t *replacement;
  int16_t samples[4];
  int close_result;
  int create_result;
  int write_result;
  int finished;
} replace_context_t;

static void *replace_thread_main(void *opaque) {
  replace_context_t *context = opaque;
  context->close_result = h2_pal_audio_track_close(context->old_track);
  context->create_result = h2_audio_mixer_create_track(
      &context->env->mixer, NULL, &TEST_TRACK_CONFIG, &context->replacement);
  h2_audio_frame_t frame = frame_for(context->samples);
  context->write_result =
      context->create_result == H2_AUDIO_OK
          ? h2_pal_audio_track_write(context->replacement, &frame, 10u)
          : H2_AUDIO_ERR_INVALID_STATE;
  control_lock(context->env);
  context->finished = 1;
  control_broadcast(context->env);
  control_unlock(context->env);
  return NULL;
}

static void test_reader_serializes_close_and_reuse(void) {
  test_env_t env;
  env_init(&env);
  const h2_audio_mixer_config_t config = mixer_config(&env, 1u);
  assert(h2_audio_mixer_init(&env.mixer, &config) == H2_AUDIO_OK);
  h2_pal_audio_track_t *track = create_track(&env);
  env.pause_recv_queue = &env.queues[0];
  env.watch_contended_lock = 1;

  read_context_t reader = {.env = &env, .result = H2_AUDIO_ERR_IO};
  pthread_t read_thread;
  assert(pthread_create(&read_thread, NULL, read_thread_main, &reader) == 0);
  control_lock(&env);
  while (!env.recv_entered) {
    control_wait(&env);
  }
  control_unlock(&env);

  replace_context_t replacer = {
      .env = &env,
      .old_track = track,
      .samples = {-11, 22, -33, 44},
      .close_result = H2_AUDIO_ERR_IO,
      .create_result = H2_AUDIO_ERR_IO,
      .write_result = H2_AUDIO_ERR_IO,
  };
  pthread_t replace_thread;
  assert(pthread_create(&replace_thread, NULL, replace_thread_main,
                        &replacer) == 0);
  control_lock(&env);
  while (!env.contended_lock_entered && !replacer.finished) {
    control_wait(&env);
  }
  assert(env.contended_lock_entered);
  assert(!replacer.finished);
  env.release_recv = 1;
  control_broadcast(&env);
  control_unlock(&env);
  assert(pthread_join(read_thread, NULL) == 0);
  assert(pthread_join(replace_thread, NULL) == 0);
  assert(reader.result == H2_AUDIO_OK);
  assert(replacer.close_result == H2_AUDIO_OK);
  assert(replacer.create_result == H2_AUDIO_OK);
  assert(replacer.write_result == H2_AUDIO_OK);

  int16_t mixed[4] = {0};
  h2_audio_frame_t frame = frame_for(mixed);
  assert(h2_audio_mixer_read(&env.mixer, &frame) == H2_AUDIO_OK);
  assert(memcmp(mixed, replacer.samples, sizeof(mixed)) == 0);
  assert(h2_pal_audio_track_close(replacer.replacement) == H2_AUDIO_OK);
  env_deinit(&env);
}

typedef struct operation_context {
  h2_pal_audio_track_t *track;
  int drain;
  int result;
} operation_context_t;

static void *operation_thread_main(void *opaque) {
  operation_context_t *context = opaque;
  if (context->drain) {
    context->result = h2_pal_audio_track_drain(context->track, 10u);
  } else {
    int16_t samples[4] = {7, 8, 9, 10};
    h2_audio_frame_t frame = frame_for(samples);
    context->result = h2_pal_audio_track_write(context->track, &frame, 10u);
  }
  return NULL;
}

typedef struct close_context {
  test_env_t *env;
  h2_pal_audio_track_t *track;
  int result;
  int finished;
  int watch_wait;
} close_context_t;

static void *close_thread_main(void *opaque) {
  close_context_t *context = opaque;
  if (context->watch_wait) {
    control_lock(context->env);
    context->env->watched_thread = pthread_self();
    context->env->watch_thread_wait = 1;
    control_broadcast(context->env);
    control_unlock(context->env);
  }
  context->result = h2_pal_audio_track_close(context->track);
  control_lock(context->env);
  context->finished = 1;
  control_broadcast(context->env);
  control_unlock(context->env);
  return NULL;
}

static void test_other_track_runs_while_close_waits(int drain,
                                                    int fail_wakeup) {
  test_env_t env;
  env_init(&env);
  const h2_audio_mixer_config_t config = mixer_config(&env, 2u);
  assert(h2_audio_mixer_init(&env.mixer, &config) == H2_AUDIO_OK);
  h2_pal_audio_track_t *closing_track = create_track(&env);
  h2_pal_audio_track_t *other_track = create_track(&env);
  env.pause_send_queue = drain ? NULL : &env.queues[0];
  env.pause_recv_queue = drain ? &env.queues[1] : NULL;

  operation_context_t operation = {
      .track = closing_track,
      .drain = drain,
      .result = H2_AUDIO_ERR_IO,
  };
  pthread_t operation_thread;
  assert(pthread_create(&operation_thread, NULL, operation_thread_main,
                        &operation) == 0);
  control_lock(&env);
  while (drain ? !env.recv_entered : !env.send_entered) {
    control_wait(&env);
  }
  control_unlock(&env);

  close_context_t closer = {
      .env = &env,
      .track = closing_track,
      .result = H2_AUDIO_ERR_IO,
  };
  pthread_t close_thread;
  assert(pthread_create(&close_thread, NULL, close_thread_main, &closer) == 0);
  control_lock(&env);
  while (env.queue_close_count < 2) {
    control_wait(&env);
  }
  assert(!closer.finished);
  control_unlock(&env);

  close_context_t retry_closer = {
      .env = &env,
      .track = closing_track,
      .result = H2_AUDIO_ERR_IO,
      .watch_wait = 1,
  };
  pthread_t retry_close_thread;
  if (fail_wakeup) {
    assert(pthread_create(&retry_close_thread, NULL, close_thread_main,
                          &retry_closer) == 0);
    control_lock(&env);
    while (!env.watched_wait_entered) {
      control_wait(&env);
    }
    env.fail_cond_broadcast = 2;
    control_unlock(&env);
  }

  int16_t samples[4] = {301, -302, 303, -304};
  h2_audio_frame_t frame = frame_for(samples);
  assert(h2_pal_audio_track_write(other_track, &frame, 10u) == H2_AUDIO_OK);
  int16_t mixed[4] = {0};
  frame = frame_for(mixed);
  assert(h2_audio_mixer_read(&env.mixer, &frame) == H2_AUDIO_OK);
  assert(memcmp(mixed, samples, sizeof(mixed)) == 0);

  control_lock(&env);
  env.release_send = 1;
  env.release_recv = 1;
  control_broadcast(&env);
  control_unlock(&env);
  assert(pthread_join(operation_thread, NULL) == 0);
  assert(pthread_join(close_thread, NULL) == 0);
  assert(operation.result == H2_AUDIO_ERR_INVALID_STATE);
  assert(closer.result == (fail_wakeup ? H2_AUDIO_ERR_IO : H2_AUDIO_OK));
  if (fail_wakeup) {
    assert(pthread_join(retry_close_thread, NULL) == 0);
    assert(retry_closer.result == H2_AUDIO_OK);
  }
  assert(h2_pal_audio_track_close(other_track) == H2_AUDIO_OK);
  env_deinit(&env);
}

int main(void) {
  test_init_and_close_failures();
  test_reader_serializes_close_and_reuse();
  test_other_track_runs_while_close_waits(0, 0);
  test_other_track_runs_while_close_waits(1, 0);
  test_other_track_runs_while_close_waits(0, 1);
  puts("PASS h2_audio_mixer concurrency");
  return 0;
}
