#include "h2_gizclaw_audio_pacer.h"
#include "h2_gizclaw_pcm_ring.h"
#include "h2_gizclaw_pcm_track_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

typedef struct memory {
  size_t attempts, fail_at, live;
} memory_t;

static void* allocate(void* user, size_t len) {
  memory_t* memory = user;
  if (++memory->attempts == memory->fail_at) return NULL;
  void* ptr = malloc(len);
  if (ptr != NULL) ++memory->live;
  return ptr;
}

static void release(void* user, void* ptr) {
  memory_t* memory = user;
  if (ptr != NULL) {
    assert(memory->live > 0u);
    --memory->live;
    free(ptr);
  }
}

static const h2_pal_mem_vtable_t memory_vtable = {.alloc = allocate,
                                                  .free = release};

static void boundaries(const h2_pal_mem_api_t* allocator) {
  h2_gizclaw_pcm_track_config_t config = {
      .allocator = allocator, .uplink_capacity = 8u, .downlink_capacity = 16u};
  h2_gizclaw_track_t* track = NULL;
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  const uint8_t input[16] = {0, 1, 2,  3,  4,  5,  6,  7,
                             8, 9, 10, 11, 12, 13, 14, 15};
  uint8_t output[16];
  memset(output, 0xa5, sizeof(output));
  size_t len = 123u;
  assert(track->vtable->read(track->user, output, 8u, &len) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(len == 0u && output[0] == 0xa5);
  assert(h2_gizclaw_pcm_track_read(track, output, 16u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_gizclaw_pcm_track_write(track, input, 6u) == H2_PAL_OK);
  assert(track->vtable->read(track->user, output, 8u, &len) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(len == 0u && output[0] == 0xa5);
  assert(h2_gizclaw_pcm_track_write(track, input + 6u, 4u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_gizclaw_pcm_track_write(track, input + 6u, 2u) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_write(track, input, 2u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(track->vtable->read(track->user, output, 6u, &len) == H2_PAL_OK);
  assert(len == 6u && memcmp(input, output, 6u) == 0);
  assert(h2_gizclaw_pcm_track_write(track, input + 8u, 6u) == H2_PAL_OK);
  assert(track->vtable->read(track->user, output, 8u, &len) == H2_PAL_OK);
  assert(len == 8u && memcmp(input + 6u, output, 8u) == 0);
  /* The other direction is independent, and never overwrites the oldest PCM. */
  assert(track->vtable->write(track->user, input, 16u) == H2_PAL_OK);
  assert(track->vtable->write(track->user, input, 2u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_gizclaw_pcm_track_read(track, output, 6u) == H2_PAL_OK);
  assert(memcmp(input, output, 6u) == 0);
  assert(track->vtable->write(track->user, input, 6u) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_read(track, output, 16u) == H2_PAL_OK);
  assert(memcmp(output, input + 6u, 10u) == 0 &&
         memcmp(output + 10u, input, 6u) == 0);
  assert(h2_gizclaw_pcm_track_write(track, input, 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_track_write(track, input, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_track_write(track, input, 10u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_track_read(track, output, 3u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_track_read(track, NULL, 2u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_track_attach_internal(track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_attach_internal(track) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_BUSY);
  assert(track != NULL);
  h2_gizclaw_pcm_track_detach_internal(track);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK && track == NULL);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_destroy(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_track_write(NULL, input, 2u) == H2_PAL_ERR_INVALID_ARG);
  for (size_t capacity = 0; capacity < 10u; ++capacity) {
    if (capacity == 0u || capacity == 2u || capacity == 4u || capacity == 8u)
      continue;
    config.uplink_capacity = capacity;
    track = (void*)1;
    assert(h2_gizclaw_pcm_track_create(&config, &track) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(track == NULL);
  }
}

typedef struct lane {
  h2_gizclaw_track_t* track;
  bool downlink;
} lane_t;

enum { TRANSFER_BYTES = 42 * 10000 };

static void* producer(void* user) {
  lane_t* lane = user;
  for (size_t offset = 0; offset < TRANSFER_BYTES;) {
    uint8_t data[14];
    for (size_t i = 0; i < sizeof(data); ++i)
      data[i] = (uint8_t)(offset + i + (lane->downlink ? 77u : 0u));
    int rc =
        lane->downlink
            ? lane->track->vtable->write(lane->track->user, data, sizeof(data))
            : h2_gizclaw_pcm_track_write(lane->track, data, sizeof(data));
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      sched_yield();
    else {
      assert(rc == H2_PAL_OK);
      offset += sizeof(data);
    }
  }
  return NULL;
}

static void* consumer(void* user) {
  lane_t* lane = user;
  for (size_t offset = 0; offset < TRANSFER_BYTES;) {
    uint8_t data[6];
    size_t len = 0;
    int rc = lane->downlink
                 ? h2_gizclaw_pcm_track_read(lane->track, data, sizeof(data))
                 : lane->track->vtable->read(lane->track->user, data,
                                             sizeof(data), &len);
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      sched_yield();
    else {
      assert(rc == H2_PAL_OK);
      assert(lane->downlink || len == sizeof(data));
      for (size_t i = 0; i < sizeof(data); ++i)
        assert(data[i] == (uint8_t)(offset + i + (lane->downlink ? 77u : 0u)));
      offset += sizeof(data);
    }
  }
  return NULL;
}

static void concurrency(const h2_pal_mem_api_t* allocator) {
  const h2_gizclaw_pcm_track_config_t config = {
      .allocator = allocator, .uplink_capacity = 32u, .downlink_capacity = 64u};
  h2_gizclaw_track_t* track = NULL;
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  lane_t lanes[2] = {{track, false}, {track, true}};
  pthread_t threads[4];
  for (size_t i = 0; i < 2u; ++i) {
    assert(pthread_create(&threads[i * 2], NULL, producer, &lanes[i]) == 0);
    assert(pthread_create(&threads[i * 2 + 1], NULL, consumer, &lanes[i]) == 0);
  }
  for (size_t i = 0; i < 4u; ++i) assert(pthread_join(threads[i], NULL) == 0);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void counter_wrap(const h2_pal_mem_api_t* allocator) {
  h2_gizclaw_pcm_ring_t ring;
  assert(h2_gizclaw_pcm_ring_init(&ring, allocator, 8u) == H2_PAL_OK);
  h2_atomic_size_store(&ring.read_index, SIZE_MAX - 3u, H2_ATOMIC_SEQ_CST);
  h2_atomic_size_store(&ring.write_index, SIZE_MAX - 3u, H2_ATOMIC_SEQ_CST);
  const uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t out[8];
  assert(h2_gizclaw_pcm_ring_write(&ring, data, 8u) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_ring_available(&ring) == 8u);
  assert(h2_gizclaw_pcm_ring_read(&ring, out, 8u) == H2_PAL_OK);
  assert(memcmp(data, out, 8u) == 0);
  assert(h2_gizclaw_pcm_ring_available(&ring) == 0u);
  h2_gizclaw_pcm_ring_deinit(&ring);
}

static void recording_window(const h2_pal_mem_api_t* allocator,
                             const h2_pal_sync_api_t* sync) {
  const h2_gizclaw_pcm_track_config_t config = {.allocator = allocator,
                                                .uplink_capacity = 16u,
                                                .downlink_capacity = 16u,
                                                .sync = sync};
  h2_gizclaw_track_t* track = NULL;
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  const uint8_t stale[4] = {1, 2, 3, 4};
  const uint8_t current[10] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
  const uint8_t later[16] = {99};
  h2_gizclaw_pcm_input_t input = {0};
  assert(h2_gizclaw_pcm_input_end(&input, track) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_pcm_track_write(track, stale, sizeof(stale)) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_start(&input, track, allocator) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_start(&input, track, allocator) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_pcm_track_write(track, current, sizeof(current)) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_end(&input, track) == H2_PAL_OK);
  assert(!input.empty);
  assert(h2_gizclaw_pcm_track_write(track, later, 2u) == H2_PAL_OK);
  /* Neither control call steals the consumer cursor. */
  assert(h2_gizclaw_pcm_track_pending_internal(track) == 16u);
  assert(h2_gizclaw_pcm_input_end(&input, track) == H2_PAL_OK);
  memory_t* memory = allocator->user;
  memory->fail_at = memory->attempts + 1u;
  assert(h2_gizclaw_pcm_input_prepare(&input, track) == H2_PAL_ERR_NO_MEMORY);
  /* Only stale pre-start PCM was discarded. The unsaved tail stays protected.
   */
  assert(h2_gizclaw_pcm_track_pending_internal(track) == 12u);
  memory->fail_at = 0u;
  assert(h2_gizclaw_pcm_input_prepare(&input, track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_pending_internal(track) == 2u);
  /* Reuse every released byte before draining the private tail. */
  assert(h2_gizclaw_pcm_track_write(track, later, 14u) == H2_PAL_OK);
  uint8_t out[16];
  size_t len;
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 6u, &len) == H2_PAL_OK);
  assert(len == 6u && memcmp(out, current, 6u) == 0);
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 6u, &len) == H2_PAL_OK);
  assert(len == 4u && memcmp(out, current + 6u, 4u) == 0);
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 6u, &len) == H2_PAL_OK);
  assert(len == 0u && h2_gizclaw_pcm_track_pending_internal(track) == 16u);
  /* The recording window remembers its own allocator. */
  h2_gizclaw_pcm_input_deinit(&input);
  assert(h2_gizclaw_pcm_input_start(&input, track, allocator) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_prepare(&input, track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_pending_internal(track) == 0u);
  assert(h2_gizclaw_pcm_input_end(&input, track) == H2_PAL_OK);
  assert(input.empty);
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 6u, &len) == H2_PAL_OK);
  assert(len == 0u);
  h2_gizclaw_pcm_input_deinit(&input);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void default_capacity(const h2_pal_mem_api_t* allocator) {
  const h2_gizclaw_pcm_track_config_t config = {.allocator = allocator};
  h2_gizclaw_track_t* track = NULL;
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  uint8_t data[16384] = {0};
  assert(h2_gizclaw_pcm_track_write(track, data, sizeof(data)) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_write(track, data, 2u) == H2_PAL_ERR_WOULD_BLOCK);
  assert(track->vtable->write(track->user, data, sizeof(data)) == H2_PAL_OK);
  assert(track->vtable->write(track->user, data, 2u) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_gizclaw_pcm_track_read(track, data, sizeof(data)) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void tail_outlives_track(const h2_pal_mem_api_t* service_allocator,
                                const h2_pal_sync_api_t* sync) {
  memory_t track_memory = {0};
  const h2_pal_mem_api_t track_allocator = {.user = &track_memory,
                                            .vtable = &memory_vtable};
  const h2_gizclaw_pcm_track_config_t config = {.allocator = &track_allocator,
                                                .uplink_capacity = 8u,
                                                .downlink_capacity = 8u,
                                                .sync = sync};
  h2_gizclaw_track_t* track = NULL;
  h2_gizclaw_pcm_input_t input = {0};
  const uint8_t data[6] = {0, 1, 2, 3, 4, 5};
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_start(&input, track, service_allocator) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_write(track, data, sizeof(data)) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_end(&input, track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_prepare(&input, track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
  assert(track_memory.live == 0u);
  assert(input.tail_len == sizeof(data) && !input.legacy_lease &&
         memcmp(input.tail, data, sizeof(data)) == 0);
  uint8_t out[8];
  size_t len;
  assert(h2_gizclaw_pcm_input_read(&input, NULL, out, sizeof(out), &len) ==
         H2_PAL_OK);
  assert(len == sizeof(data) && memcmp(out, data, len) == 0);
  h2_gizclaw_pcm_input_deinit(&input);
}

static void discard_downlink(const h2_pal_mem_api_t* allocator) {
  h2_gizclaw_pcm_track_config_t config = {
      .allocator = allocator, .uplink_capacity = 8u, .downlink_capacity = 16u};
  h2_gizclaw_track_t* track = NULL;
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  const uint8_t stale[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  const uint8_t fresh[4] = {7, 8, 9, 10};
  uint8_t output[16];
  /* Nothing queued: a discard is harmless. */
  h2_gizclaw_pcm_track_discard_downlink_internal(track);
  assert(h2_gizclaw_pcm_track_read(track, output, 2u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  /* Stale audio queued before the request boundary never reaches the reader,
   * even when the new request already appended past the watermark. */
  assert(track->vtable->write(track->user, stale, sizeof(stale)) == H2_PAL_OK);
  h2_gizclaw_pcm_track_discard_downlink_internal(track);
  assert(track->vtable->write(track->user, fresh, sizeof(fresh)) == H2_PAL_OK);
  memset(output, 0xa5, sizeof(output));
  assert(h2_gizclaw_pcm_track_read(track, output, sizeof(fresh)) == H2_PAL_OK);
  assert(memcmp(output, fresh, sizeof(fresh)) == 0);
  assert(h2_gizclaw_pcm_track_read(track, output, 2u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  /* A discard with nothing new behind it leaves the ring empty and reusable
   * across the wrap point. */
  assert(track->vtable->write(track->user, stale, sizeof(stale)) == H2_PAL_OK);
  h2_gizclaw_pcm_track_discard_downlink_internal(track);
  assert(h2_gizclaw_pcm_track_read(track, output, 2u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(track->vtable->write(track->user, stale, sizeof(stale)) == H2_PAL_OK);
  assert(track->vtable->write(track->user, fresh, sizeof(fresh)) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_read(track, output, 12u) == H2_PAL_OK);
  assert(memcmp(output, stale, sizeof(stale)) == 0 &&
         memcmp(output + 8u, fresh, sizeof(fresh)) == 0);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

typedef struct sync_state {
  size_t live, fail_locks;
  bool fail_create, fail_lock, fail_destroy;
  void (*before_lock)(void* user);
  void* before_lock_user;
} sync_state_t;

struct h2_pal_mutex {
  pthread_mutex_t native;
  const h2_pal_mem_api_t* allocator;
};

static h2_pal_result_t create_mutex(void* user,
                                    const h2_pal_mutex_config_t* config,
                                    h2_pal_mutex_t** out_mutex) {
  sync_state_t* state = user;
  *out_mutex = NULL;
  if (state->fail_create) return H2_PAL_ERR_UNSUPPORTED;
  h2_pal_mutex_t* mutex = h2_pal_mem_alloc(config->allocator, sizeof(*mutex));
  if (mutex == NULL) return H2_PAL_ERR_NO_MEMORY;
  mutex->allocator = config->allocator;
  assert(pthread_mutex_init(&mutex->native, NULL) == 0);
  *out_mutex = mutex;
  ++state->live;
  return H2_PAL_OK;
}

static h2_pal_result_t destroy_mutex(void* user, h2_pal_mutex_t* mutex) {
  sync_state_t* state = user;
  if (state->fail_destroy) return H2_PAL_ERR_BUSY;
  assert(pthread_mutex_destroy(&mutex->native) == 0);
  h2_pal_mem_free(mutex->allocator, mutex);
  assert(state->live > 0u);
  --state->live;
  return H2_PAL_OK;
}

static h2_pal_result_t lock_mutex(void* user, h2_pal_mutex_t* mutex) {
  sync_state_t* state = user;
  if (state->before_lock != NULL) state->before_lock(state->before_lock_user);
  if (state->fail_lock) return H2_PAL_ERR_IO;
  if (state->fail_locks != 0u) {
    --state->fail_locks;
    return H2_PAL_ERR_IO;
  }
  assert(pthread_mutex_lock(&mutex->native) == 0);
  return H2_PAL_OK;
}

static h2_pal_result_t unlock_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  assert(pthread_mutex_unlock(&mutex->native) == 0);
  return H2_PAL_OK;
}

static const h2_pal_sync_vtable_t sync_vtable = {.create_mutex = create_mutex,
                                                 .destroy_mutex = destroy_mutex,
                                                 .lock_mutex = lock_mutex,
                                                 .unlock_mutex = unlock_mutex};

static h2_gizclaw_track_t* capture_track(const h2_pal_mem_api_t* allocator,
                                         const h2_pal_sync_api_t* sync,
                                         size_t capacity) {
  const h2_gizclaw_pcm_track_config_t config = {.allocator = allocator,
                                                .uplink_capacity = capacity,
                                                .downlink_capacity = capacity,
                                                .sync = sync};
  h2_gizclaw_track_t* track = NULL;
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  return track;
}

static h2_gizclaw_pcm_capture_snapshot_t capture_snapshot(
    h2_gizclaw_track_t* track, h2_gizclaw_pcm_capture_id_t id,
    h2_gizclaw_pcm_capture_state_t state) {
  h2_gizclaw_pcm_capture_snapshot_t snapshot;
  assert(h2_gizclaw_pcm_capture_snapshot(track, id, &snapshot) == H2_PAL_OK);
  assert(snapshot.state == state);
  assert(id == 0u || snapshot.id == id);
  return snapshot;
}

static void capture_submit(h2_gizclaw_track_t* track,
                           h2_gizclaw_pcm_capture_id_t id, const uint8_t* pcm,
                           size_t len) {
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_frame_submit(track, id, pcm, len, H2_PAL_OK) ==
         H2_PAL_OK);
}

static void capture_unsupported(const h2_pal_mem_api_t* allocator) {
  h2_gizclaw_track_t* track = capture_track(allocator, NULL, 8u);
  h2_gizclaw_pcm_capture_id_t id = 99u;
  h2_gizclaw_pcm_capture_snapshot_t snapshot;
  h2_gizclaw_pcm_input_t input = {0};
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_ERR_UNSUPPORTED);
  assert(id == 0u);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, 1u) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(h2_gizclaw_pcm_capture_frame_submit(track, 1u, NULL, 0u,
                                             H2_PAL_ERR_TIMEOUT) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(h2_gizclaw_pcm_capture_end(track, 1u) == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_gizclaw_pcm_capture_discard(track, 1u) == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_gizclaw_pcm_capture_snapshot(track, 0u, &snapshot) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, 1u) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(!h2_gizclaw_pcm_capture_busy_internal(track));
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void capture_before_adopt(const h2_pal_mem_api_t* allocator,
                                 const h2_pal_sync_api_t* sync) {
  h2_gizclaw_track_t* track = capture_track(allocator, sync, 16u);
  memory_t* memory = allocator->user;
  const size_t allocations = memory->attempts;
  const uint8_t ambient[4] = {99, 99, 99, 99};
  const uint8_t pcm[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  uint8_t out[32] = {0};
  size_t len = 0u;
  h2_gizclaw_pcm_input_t input = {0}, other = {0};
  h2_gizclaw_pcm_capture_id_t id, next;
  h2_gizclaw_pcm_capture_snapshot_t snapshot =
      capture_snapshot(track, 0u, H2_GIZCLAW_PCM_CAPTURE_IDLE);
  assert(snapshot.id == 0u && !snapshot.adopted && !snapshot.producer_busy &&
         !snapshot.consumer_busy && snapshot.pending_bytes == 0u &&
         snapshot.captured_bytes == 0u && snapshot.result == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_write(track, ambient, sizeof(ambient)) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK && id != 0u);
  assert(h2_gizclaw_pcm_track_pending_internal(track) == 0u);
  capture_submit(track, id, pcm, 4u);
  capture_submit(track, id, pcm + 4u, 6u);
  assert(h2_gizclaw_pcm_track_write(track, ambient, sizeof(ambient)) ==
         H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_OK);
  snapshot = capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_FROZEN);
  assert(snapshot.captured_bytes == sizeof(pcm) &&
         snapshot.pending_bytes == sizeof(pcm) && !snapshot.producer_busy &&
         !snapshot.adopted && !snapshot.consumer_busy);
  assert(h2_gizclaw_pcm_capture_busy_internal(track));
  assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_ERR_BUSY);
  assert(next == 0u);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) == H2_PAL_ERR_CLOSED);
  assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, id) == H2_PAL_OK);
  assert(input.capture_id == id && input.begin == 4u);
  assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_adopt(&other, track, allocator, id) ==
         H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_input_prepare(&input, track) == H2_PAL_OK);
  assert(input.ended && !input.empty && !input.tail_taken &&
         input.tail == NULL);
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 6u, &len) == H2_PAL_OK);
  assert(len == 6u && memcmp(out, pcm, len) == 0);
  snapshot = capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_FROZEN);
  assert(snapshot.adopted && snapshot.consumer_busy &&
         snapshot.pending_bytes == 4u && snapshot.captured_bytes == 10u);
  assert(h2_gizclaw_pcm_input_read(&input, track, out, sizeof(out), &len) ==
         H2_PAL_OK);
  assert(len == 4u && memcmp(out, pcm + 6u, len) == 0);
  assert(input.tail == NULL && input.tail_taken);
  snapshot = capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_DONE);
  assert(snapshot.pending_bytes == 0u && snapshot.consumer_busy);
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 6u, &len) == H2_PAL_OK);
  assert(len == 0u);
  assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_BUSY);
  h2_gizclaw_pcm_input_deinit(&input);
  assert(input.capture_id == 0u &&
         !h2_gizclaw_pcm_capture_busy_internal(track));
  assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_OK && next != id);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, next) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_gizclaw_pcm_capture_discard(track, id) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(h2_gizclaw_pcm_capture_frame_submit(
             track, id, pcm, sizeof(pcm), H2_PAL_OK) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_gizclaw_pcm_capture_snapshot(track, id, &snapshot) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, id) ==
         H2_PAL_ERR_NOT_FOUND);
  snapshot = capture_snapshot(track, next, H2_GIZCLAW_PCM_CAPTURE_CAPTURING);
  assert(snapshot.producer_busy && snapshot.pending_bytes == 0u &&
         snapshot.captured_bytes == 0u && !snapshot.adopted);
  assert(h2_gizclaw_pcm_capture_frame_submit(track, next, NULL, 0u,
                                             H2_PAL_ERR_TIMEOUT) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_discard(track, next) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_discard(track, next) == H2_PAL_OK);
  assert(memory->attempts ==
         allocations); /* No prefix or frozen-tail copies. */
  assert(h2_gizclaw_pcm_track_write(track, ambient, sizeof(ambient)) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_discard(track, next) == H2_PAL_OK);
  assert(track->vtable->read(track->user, out, sizeof(ambient), &len) ==
         H2_PAL_OK);
  assert(len == sizeof(ambient) && memcmp(out, ambient, len) == 0);
  snapshot = capture_snapshot(track, next, H2_GIZCLAW_PCM_CAPTURE_DISCARDED);
  assert(snapshot.pending_bytes == 0u && snapshot.captured_bytes == 0u);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void capture_streaming(const h2_pal_mem_api_t* allocator,
                              const h2_pal_sync_api_t* sync) {
  h2_gizclaw_track_t* track = capture_track(allocator, sync, 8u);
  h2_gizclaw_pcm_input_t input = {0};
  h2_gizclaw_pcm_capture_id_t id;
  const uint8_t pcm[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  uint8_t out[8];
  size_t len;
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  capture_submit(track, id, pcm, 4u);
  assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 8u, &len) == H2_PAL_OK);
  assert(len == 4u && memcmp(out, pcm, 4u) == 0 && !input.ended);
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 8u, &len) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(len == 0u && !input.ended && !input.tail_taken);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_capture_frame_submit(
             track, id, NULL, 0u, H2_PAL_ERR_WOULD_BLOCK) == H2_PAL_OK);
  capture_submit(track, id, pcm + 4u, 8u); /* Reuse bytes already consumed. */
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 6u, &len) == H2_PAL_OK);
  assert(len == 6u && memcmp(out, pcm + 4u, 6u) == 0);
  assert(h2_gizclaw_pcm_input_end(&input, track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 6u, &len) == H2_PAL_OK);
  assert(len == 2u && memcmp(out, pcm + 10u, 2u) == 0 && input.ended);
  h2_gizclaw_pcm_capture_snapshot_t snapshot =
      capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_DONE);
  assert(snapshot.captured_bytes == 12u && snapshot.pending_bytes == 0u);
  h2_gizclaw_pcm_input_deinit(&input);
  /* Empty windows still require a consumer acknowledgement or discard. */
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_prepare(&input, track) == H2_PAL_OK);
  assert(input.ended && input.empty && input.tail_taken && input.tail == NULL);
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 8u, &len) == H2_PAL_OK);
  assert(len == 0u);
  h2_gizclaw_pcm_input_deinit(&input);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void capture_legacy_isolation(const h2_pal_mem_api_t* allocator,
                                     const h2_pal_sync_api_t* sync) {
  h2_gizclaw_track_t* track = capture_track(allocator, sync, 16u);
  h2_gizclaw_pcm_input_t placeholder = {0}, other = {0};
  h2_gizclaw_pcm_capture_id_t id;
  const uint8_t pcm[6] = {1, 2, 3, 4, 5, 6};
  uint8_t out[8];
  size_t len;
  assert(h2_gizclaw_pcm_input_start(&placeholder, track, allocator) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  capture_submit(track, id, pcm, sizeof(pcm));
  assert(h2_gizclaw_pcm_input_prepare(&placeholder, track) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(!placeholder.begun);
  assert(h2_gizclaw_pcm_input_read(&placeholder, track, out, 4u, &len) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_gizclaw_pcm_input_end(&placeholder, track) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(!placeholder.ended);
  assert(h2_gizclaw_pcm_input_start(&other, track, allocator) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(track->vtable->read(track->user, out, 4u, &len) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_gizclaw_pcm_track_pending_internal(track) == sizeof(pcm));
  assert(h2_gizclaw_pcm_input_adopt(&placeholder, track, allocator, id) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_read(&placeholder, track, out, 8u, &len) ==
         H2_PAL_OK);
  assert(len == sizeof(pcm) && memcmp(out, pcm, sizeof(pcm)) == 0);
  h2_gizclaw_pcm_input_deinit(&placeholder);
  /* A begun or ended legacy window cannot silently become a different input. */
  assert(h2_gizclaw_pcm_input_start(&other, track, allocator) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_prepare(&other, track) == H2_PAL_OK);
  h2_gizclaw_pcm_capture_id_t next;
  assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_ERR_BUSY);
  assert(next == 0u);
  assert(h2_gizclaw_pcm_input_adopt(&other, track, allocator, id) ==
         H2_PAL_ERR_INVALID_STATE);
  h2_gizclaw_pcm_input_deinit(&other);
  assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_discard(track, next) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void capture_preserves_legacy_window(const h2_pal_mem_api_t* allocator,
                                            const h2_pal_sync_api_t* sync) {
  h2_gizclaw_track_t* track = capture_track(allocator, sync, 16u);
  h2_gizclaw_pcm_input_t legacy = {0}, managed = {0};
  h2_gizclaw_pcm_capture_id_t id, next;
  const uint8_t old_pcm[6] = {1, 2, 3, 4, 5, 6};
  const uint8_t new_pcm[4] = {7, 8, 9, 10};
  uint8_t out[8];
  size_t len;
  assert(h2_gizclaw_pcm_input_start(&legacy, track, allocator) == H2_PAL_OK);
  assert(legacy.legacy_lease);
  assert(h2_gizclaw_pcm_track_write(track, old_pcm, sizeof(old_pcm)) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_ERR_BUSY);
  assert(id == 0u);
  assert(h2_gizclaw_pcm_input_end(&legacy, track) == H2_PAL_OK);
  assert(!h2_gizclaw_pcm_capture_busy_internal(track));
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_BUSY);
  memory_t* memory = allocator->user;
  memory->fail_at = memory->attempts + 1u;
  assert(h2_gizclaw_pcm_input_prepare(&legacy, track) == H2_PAL_ERR_NO_MEMORY);
  assert(legacy.legacy_lease && !legacy.tail_taken);
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_track_pending_internal(track) == sizeof(old_pcm));
  memory->fail_at = 0u;
  assert(h2_gizclaw_pcm_input_prepare(&legacy, track) == H2_PAL_OK);
  assert(!legacy.legacy_lease && legacy.tail_taken);
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  capture_submit(track, id, new_pcm, sizeof(new_pcm));
  /* The old tail can drain after the ring belongs to a new capture. */
  assert(h2_gizclaw_pcm_input_prepare(&legacy, track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_read(&legacy, track, out, sizeof(out), &len) ==
         H2_PAL_OK);
  assert(len == sizeof(old_pcm) && memcmp(out, old_pcm, len) == 0);
  h2_gizclaw_pcm_input_deinit(&legacy);
  h2_gizclaw_pcm_capture_snapshot_t snapshot =
      capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_CAPTURING);
  assert(snapshot.pending_bytes == sizeof(new_pcm) && !snapshot.consumer_busy);
  assert(h2_gizclaw_pcm_input_adopt(&managed, track, allocator, id) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_read(&managed, track, out, sizeof(out), &len) ==
         H2_PAL_OK);
  assert(len == sizeof(new_pcm) && memcmp(out, new_pcm, len) == 0);
  h2_gizclaw_pcm_input_deinit(&managed);

  /* An unadopted empty START is invalidated, including after capture drains. */
  assert(h2_gizclaw_pcm_input_start(&legacy, track, allocator) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  capture_submit(track, id, new_pcm, sizeof(new_pcm));
  assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_adopt(&managed, track, allocator, id) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_read(&managed, track, out, sizeof(out), &len) ==
         H2_PAL_OK);
  h2_gizclaw_pcm_input_deinit(&managed);
  assert(h2_gizclaw_pcm_track_write(track, new_pcm, sizeof(new_pcm)) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_prepare(&legacy, track) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_pcm_input_read(&legacy, track, out, sizeof(out), &len) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(len == 0u);
  assert(h2_gizclaw_pcm_input_end(&legacy, track) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_pcm_track_pending_internal(track) == sizeof(new_pcm));
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_OK && next != id);
  capture_submit(track, next, old_pcm, sizeof(old_pcm));
  assert(h2_gizclaw_pcm_input_adopt(&managed, track, allocator, next) ==
         H2_PAL_OK);
  h2_gizclaw_pcm_input_deinit(&legacy);
  snapshot = capture_snapshot(track, next, H2_GIZCLAW_PCM_CAPTURE_CAPTURING);
  assert(snapshot.consumer_busy && snapshot.pending_bytes == sizeof(old_pcm));
  assert(h2_gizclaw_pcm_capture_end(track, next) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_read(&managed, track, out, sizeof(out), &len) ==
         H2_PAL_OK);
  assert(len == sizeof(old_pcm) && memcmp(out, old_pcm, len) == 0);
  h2_gizclaw_pcm_input_deinit(&managed);

  /* Retirement, without tail save, also relinquishes an accepted legacy window.
   */
  assert(h2_gizclaw_pcm_input_start(&legacy, track, allocator) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_write(track, old_pcm, sizeof(old_pcm)) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_end(&legacy, track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_ERR_BUSY);
  h2_gizclaw_pcm_input_deinit(&legacy);
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_discard(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void capture_failures(const h2_pal_mem_api_t* allocator,
                             const h2_pal_sync_api_t* sync) {
  h2_gizclaw_track_t* track = capture_track(allocator, sync, 8u);
  h2_gizclaw_pcm_capture_id_t id, next;
  h2_gizclaw_pcm_input_t input = {0};
  const uint8_t pcm[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  uint8_t out[8];
  size_t len;
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, id) == H2_PAL_OK);
  capture_submit(track, id, pcm, 8u);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_frame_submit(track, id, pcm + 8u, 2u,
                                             H2_PAL_OK) == H2_PAL_ERR_NO_SPACE);
  h2_gizclaw_pcm_capture_snapshot_t snapshot =
      capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_FAILED);
  assert(snapshot.result == H2_PAL_ERR_NO_SPACE &&
         snapshot.pending_bytes == 8u && snapshot.captured_bytes == 8u &&
         !snapshot.producer_busy);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) ==
         H2_PAL_ERR_NO_SPACE);
  assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_ERR_NO_SPACE);
  memset(out, 0xa5, sizeof(out));
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 8u, &len) ==
         H2_PAL_ERR_NO_SPACE);
  assert(len == 0u && out[0] == 0xa5);
  assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_capture_discard(track, id) == H2_PAL_OK);
  snapshot = capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_DISCARDED);
  assert(snapshot.result == H2_PAL_ERR_NO_SPACE &&
         snapshot.pending_bytes == 0u);
  assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_ERR_BUSY);
  h2_gizclaw_pcm_input_deinit(&input);
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_frame_submit(track, id, pcm, sizeof(pcm),
                                             H2_PAL_OK) == H2_PAL_ERR_NO_SPACE);
  snapshot = capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_FAILED);
  assert(snapshot.pending_bytes == 0u && snapshot.captured_bytes == 0u);
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_frame_submit(track, id, NULL, 0u,
                                             H2_PAL_ERR_IO) == H2_PAL_ERR_IO);
  snapshot = capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_FAILED);
  assert(snapshot.result == H2_PAL_ERR_IO && !snapshot.producer_busy);
  assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, id) ==
         H2_PAL_ERR_IO);
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_frame_submit(track, id, pcm, 3u, H2_PAL_OK) ==
         H2_PAL_ERR_INVALID_ARG);
  snapshot = capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_FAILED);
  assert(snapshot.result == H2_PAL_ERR_INVALID_ARG && !snapshot.producer_busy);
  assert(h2_gizclaw_pcm_capture_frame_submit(track, id, pcm, 2u, H2_PAL_OK) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void capture_malformed_transient(const h2_pal_mem_api_t* allocator,
                                        const h2_pal_sync_api_t* sync) {
  h2_gizclaw_track_t* track = capture_track(allocator, sync, 8u);
  const h2_pal_result_t transient[] = {H2_PAL_ERR_WOULD_BLOCK,
                                       H2_PAL_ERR_TIMEOUT};
  const uint8_t pcm[2] = {1, 2};
  for (size_t result = 0u; result < 2u; ++result) {
    for (unsigned combination = 0u; combination < 3u; ++combination) {
      h2_gizclaw_pcm_capture_id_t id;
      h2_gizclaw_pcm_input_t input = {0};
      assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
      assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, id) ==
             H2_PAL_OK);
      assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) == H2_PAL_OK);
      if (combination == 2u)
        assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_OK);
      const uint8_t* bytes = combination == 0u ? NULL : pcm;
      const size_t size = combination == 1u ? 0u : sizeof(pcm);
      assert(h2_gizclaw_pcm_capture_frame_submit(track, id, bytes, size,
                                                 transient[result]) ==
             H2_PAL_ERR_INVALID_ARG);
      h2_gizclaw_pcm_capture_snapshot_t snapshot =
          capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_FAILED);
      assert(snapshot.result == H2_PAL_ERR_INVALID_ARG &&
             !snapshot.producer_busy && snapshot.pending_bytes == 0u);
      uint8_t out[2] = {99, 99};
      size_t len;
      assert(h2_gizclaw_pcm_input_read(&input, track, out, sizeof(out), &len) ==
             H2_PAL_ERR_INVALID_ARG);
      assert(len == 0u && out[0] == 99);
      assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_ERR_INVALID_ARG);
      h2_gizclaw_pcm_input_deinit(&input);
    }
    h2_gizclaw_pcm_capture_id_t id;
    assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
    assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) == H2_PAL_OK);
    assert(h2_gizclaw_pcm_capture_frame_submit(track, id, NULL, 0u,
                                               transient[result]) == H2_PAL_OK);
    h2_gizclaw_pcm_capture_snapshot_t snapshot =
        capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_CAPTURING);
    assert(snapshot.result == H2_PAL_OK && !snapshot.producer_busy);
    assert(h2_gizclaw_pcm_capture_discard(track, id) == H2_PAL_OK);
  }
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

typedef struct delayed_frame {
  h2_gizclaw_track_t* track;
  h2_gizclaw_pcm_capture_id_t id;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool reading, finish;
} delayed_frame_t;

static void* delayed_frame_producer(void* user) {
  delayed_frame_t* frame = user;
  assert(h2_gizclaw_pcm_capture_frame_acquire(frame->track, frame->id) ==
         H2_PAL_OK);
  /* Model a PAL device read which sleeps while control handles END/DISCARD. */
  assert(pthread_mutex_lock(&frame->mutex) == 0);
  frame->reading = true;
  assert(pthread_cond_signal(&frame->cond) == 0);
  while (!frame->finish)
    assert(pthread_cond_wait(&frame->cond, &frame->mutex) == 0);
  assert(pthread_mutex_unlock(&frame->mutex) == 0);
  const uint8_t pcm[6] = {1, 2, 3, 4, 5, 6};
  assert(h2_gizclaw_pcm_capture_frame_submit(frame->track, frame->id, pcm,
                                             sizeof(pcm),
                                             H2_PAL_OK) == H2_PAL_OK);
  return NULL;
}

static void capture_stop_during_frame(const h2_pal_mem_api_t* allocator,
                                      const h2_pal_sync_api_t* sync) {
  h2_gizclaw_track_t* track = capture_track(allocator, sync, 8u);
  for (unsigned discard = 0u; discard < 2u; ++discard) {
    h2_gizclaw_pcm_capture_id_t id, next;
    h2_gizclaw_pcm_input_t input = {0};
    assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
    if (!discard)
      assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, id) ==
             H2_PAL_OK);
    delayed_frame_t frame = {.track = track, .id = id};
    assert(pthread_mutex_init(&frame.mutex, NULL) == 0);
    assert(pthread_cond_init(&frame.cond, NULL) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, delayed_frame_producer, &frame) == 0);
    assert(pthread_mutex_lock(&frame.mutex) == 0);
    while (!frame.reading)
      assert(pthread_cond_wait(&frame.cond, &frame.mutex) == 0);
    assert(pthread_mutex_unlock(&frame.mutex) == 0);
    assert((discard ? h2_gizclaw_pcm_capture_discard(track, id)
                    : h2_gizclaw_pcm_capture_end(track, id)) == H2_PAL_OK);
    assert((discard ? h2_gizclaw_pcm_capture_discard(track, id)
                    : h2_gizclaw_pcm_capture_end(track, id)) == H2_PAL_OK);
    h2_gizclaw_pcm_capture_snapshot_t snapshot =
        capture_snapshot(track, id,
                         discard ? H2_GIZCLAW_PCM_CAPTURE_DISCARDING
                                 : H2_GIZCLAW_PCM_CAPTURE_STOPPING);
    assert(snapshot.producer_busy && snapshot.pending_bytes == 0u);
    assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_ERR_BUSY);
    assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_BUSY);
    uint8_t out[8];
    size_t len;
    if (!discard) {
      assert(h2_gizclaw_pcm_input_read(&input, track, out, 8u, &len) ==
             H2_PAL_ERR_WOULD_BLOCK);
      assert(len == 0u && !input.ended && !input.empty && !input.tail_taken);
    }
    assert(pthread_mutex_lock(&frame.mutex) == 0);
    frame.finish = true;
    assert(pthread_cond_signal(&frame.cond) == 0);
    assert(pthread_mutex_unlock(&frame.mutex) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(pthread_cond_destroy(&frame.cond) == 0);
    assert(pthread_mutex_destroy(&frame.mutex) == 0);
    snapshot = capture_snapshot(track, id,
                                discard ? H2_GIZCLAW_PCM_CAPTURE_DISCARDED
                                        : H2_GIZCLAW_PCM_CAPTURE_FROZEN);
    assert(!snapshot.producer_busy);
    if (!discard) {
      assert(snapshot.pending_bytes == 6u && snapshot.captured_bytes == 6u);
      assert(h2_gizclaw_pcm_input_read(&input, track, out, 8u, &len) ==
             H2_PAL_OK);
      const uint8_t expected[6] = {1, 2, 3, 4, 5, 6};
      assert(len == 6u && memcmp(out, expected, 6u) == 0 && input.ended &&
             !input.empty && input.tail_taken);
      h2_gizclaw_pcm_input_deinit(&input);
    } else {
      assert(snapshot.pending_bytes == 0u && snapshot.captured_bytes == 0u &&
             !snapshot.adopted);
      assert(!h2_gizclaw_pcm_capture_busy_internal(track));
      /* A short unadopted recording can restart immediately after frame ACK. */
      assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_OK);
      assert(h2_gizclaw_pcm_capture_discard(track, next) == H2_PAL_OK);
    }
  }
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void capture_reader_cleanup(const h2_pal_mem_api_t* allocator,
                                   const h2_pal_sync_api_t* sync) {
  h2_gizclaw_track_t* track = capture_track(allocator, sync, 8u);
  h2_gizclaw_pcm_input_t input = {0};
  h2_gizclaw_pcm_capture_id_t id, next;
  const uint8_t pcm[4] = {1, 2, 3, 4};
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  capture_submit(track, id, pcm, sizeof(pcm));
  assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) == H2_PAL_OK);
  h2_gizclaw_pcm_input_deinit(&input);
  h2_gizclaw_pcm_capture_snapshot_t snapshot =
      capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_DISCARDING);
  assert(snapshot.adopted && !snapshot.consumer_busy &&
         snapshot.producer_busy && snapshot.pending_bytes == 0u);
  assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_capture_frame_submit(track, id, pcm, sizeof(pcm),
                                             H2_PAL_OK) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, next) ==
         H2_PAL_OK);
  capture_submit(track, next, pcm, sizeof(pcm));
  assert(h2_gizclaw_pcm_capture_end(track, next) == H2_PAL_OK);
  h2_gizclaw_pcm_input_deinit(&input); /* Cancel an unread frozen tail. */
  snapshot = capture_snapshot(track, next, H2_GIZCLAW_PCM_CAPTURE_DONE);
  assert(!snapshot.consumer_busy && snapshot.pending_bytes == 0u);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

typedef struct reader_deinit_gate {
  pthread_mutex_t mutex;
  pthread_cond_t progress;
  h2_gizclaw_pcm_input_t* input;
  bool entered, resume;
} reader_deinit_gate_t;
static _Thread_local bool reader_deinit_task;

static void pause_reader_deinit(void* user) {
  if (!reader_deinit_task) return;
  reader_deinit_gate_t* gate = user;
  assert(pthread_mutex_lock(&gate->mutex) == 0);
  gate->entered = true;
  assert(pthread_cond_broadcast(&gate->progress) == 0);
  while (!gate->resume)
    assert(pthread_cond_wait(&gate->progress, &gate->mutex) == 0);
  assert(pthread_mutex_unlock(&gate->mutex) == 0);
}
static void* reader_deinit_entry(void* user) {
  reader_deinit_gate_t* gate = user;
  reader_deinit_task = true;
  h2_gizclaw_pcm_input_deinit(gate->input);
  reader_deinit_task = false;
  return NULL;
}

static void capture_reader_deinit_pins_track(const h2_pal_mem_api_t* allocator,
                                             const h2_pal_sync_api_t* sync) {
  h2_gizclaw_track_t* track = capture_track(allocator, sync, 8u);
  h2_gizclaw_pcm_input_t input = {0};
  h2_gizclaw_pcm_capture_id_t id, next;
  const uint8_t pcm[2] = {1, 2};
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  capture_submit(track, id, pcm, sizeof(pcm));
  assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, id) == H2_PAL_OK);
  reader_deinit_gate_t gate = {.input = &input};
  assert(pthread_mutex_init(&gate.mutex, NULL) == 0);
  assert(pthread_cond_init(&gate.progress, NULL) == 0);
  sync_state_t* state = sync->user;
  state->before_lock = pause_reader_deinit;
  state->before_lock_user = &gate;
  pthread_t reader;
  assert(pthread_create(&reader, NULL, reader_deinit_entry, &gate) == 0);
  assert(pthread_mutex_lock(&gate.mutex) == 0);
  while (!gate.entered)
    assert(pthread_cond_wait(&gate.progress, &gate.mutex) == 0);
  assert(pthread_mutex_unlock(&gate.mutex) == 0);

  /* Deinit is paused before acquiring the PAL mutex. Another Track user must
   * not collect its lease and allow destruction while it still needs Track. */
  const h2_gizclaw_pcm_capture_snapshot_t snapshot =
      capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_FROZEN);
  assert(snapshot.consumer_busy && snapshot.pending_bytes == sizeof(pcm));
  assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_BUSY);

  assert(pthread_mutex_lock(&gate.mutex) == 0);
  gate.resume = true;
  assert(pthread_cond_broadcast(&gate.progress) == 0);
  assert(pthread_mutex_unlock(&gate.mutex) == 0);
  assert(pthread_join(reader, NULL) == 0);
  state->before_lock = NULL;
  state->before_lock_user = NULL;
  assert(input.track == NULL && !input.active && input.capture_id == 0u);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
  assert(pthread_cond_destroy(&gate.progress) == 0);
  assert(pthread_mutex_destroy(&gate.mutex) == 0);
}

static void capture_reader_retirement_retry(const h2_pal_mem_api_t* allocator,
                                            const h2_pal_sync_api_t* sync) {
  sync_state_t* state = sync->user;
  const uint8_t pcm[4] = {1, 2, 3, 4};
  /* Capturing, frozen, in-flight producer, drained, and failed readers. */
  for (unsigned mode = 0u; mode < 5u; ++mode) {
    h2_gizclaw_track_t* track = capture_track(allocator, sync, 8u);
    h2_gizclaw_pcm_input_t* input = calloc(1u, sizeof(*input));
    assert(input != NULL);
    h2_gizclaw_pcm_capture_id_t id, next;
    assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
    assert(h2_gizclaw_pcm_input_adopt(input, track, allocator, id) ==
           H2_PAL_OK);
    capture_submit(track, id, pcm, sizeof(pcm));
    if (mode == 1u || mode == 3u)
      assert(h2_gizclaw_pcm_capture_end(track, id) == H2_PAL_OK);
    if (mode == 2u || mode == 4u)
      assert(h2_gizclaw_pcm_capture_frame_acquire(track, id) == H2_PAL_OK);
    if (mode == 3u) {
      uint8_t out[8];
      size_t len;
      assert(h2_gizclaw_pcm_input_read(input, track, out, sizeof(out), &len) ==
             H2_PAL_OK);
      assert(len == sizeof(pcm));
    }
    if (mode == 4u)
      assert(h2_gizclaw_pcm_capture_frame_submit(
                 track, id, NULL, 0u, H2_PAL_ERR_IO) == H2_PAL_ERR_IO);
    state->fail_locks = 1u;
    h2_gizclaw_pcm_input_deinit(input);
    assert(state->fail_locks == 0u && input->track == NULL && !input->active &&
           input->capture_id == 0u);
    free(input); /* No subsequent operation may inspect this retired owner. */
    if (mode == 1u || mode == 3u || mode == 4u) {
      assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
      continue;
    }
    if (mode == 2u) {
      assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_ERR_BUSY);
      h2_gizclaw_pcm_capture_snapshot_t snapshot =
          capture_snapshot(track, id, H2_GIZCLAW_PCM_CAPTURE_DISCARDING);
      assert(snapshot.producer_busy && !snapshot.consumer_busy &&
             snapshot.pending_bytes == 0u);
      assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_BUSY);
      assert(h2_gizclaw_pcm_capture_frame_submit(track, id, pcm, sizeof(pcm),
                                                 H2_PAL_OK) == H2_PAL_OK);
    }
    assert(h2_gizclaw_pcm_capture_begin(track, &next) == H2_PAL_OK &&
           next != id);
    assert(h2_gizclaw_pcm_capture_frame_acquire(track, next) == H2_PAL_OK);
    assert(h2_gizclaw_pcm_capture_frame_submit(
               track, id, pcm, sizeof(pcm), H2_PAL_OK) == H2_PAL_ERR_NOT_FOUND);
    h2_gizclaw_pcm_capture_snapshot_t snapshot =
        capture_snapshot(track, next, H2_GIZCLAW_PCM_CAPTURE_CAPTURING);
    assert(snapshot.producer_busy && !snapshot.consumer_busy &&
           snapshot.pending_bytes == 0u);
    assert(h2_gizclaw_pcm_capture_frame_submit(
               track, next, NULL, 0u, H2_PAL_ERR_TIMEOUT) == H2_PAL_OK);
    assert(h2_gizclaw_pcm_capture_discard(track, next) == H2_PAL_OK);
    assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
  }
}

static void legacy_reader_retirement_retry(const h2_pal_mem_api_t* allocator,
                                           const h2_pal_sync_api_t* sync) {
  sync_state_t* state = sync->user;
  const uint8_t pcm[4] = {1, 2, 3, 4};
  for (unsigned destroy_first = 0u; destroy_first < 2u; ++destroy_first) {
    h2_gizclaw_track_t* track = capture_track(allocator, sync, 8u);
    h2_gizclaw_pcm_input_t* input = calloc(1u, sizeof(*input));
    assert(input != NULL);
    assert(h2_gizclaw_pcm_input_start(input, track, allocator) == H2_PAL_OK);
    assert(h2_gizclaw_pcm_track_write(track, pcm, sizeof(pcm)) == H2_PAL_OK);
    assert(h2_gizclaw_pcm_input_end(input, track) == H2_PAL_OK);
    /* Service may unbind a legacy input; the remaining lifetime pin is local.
     */
    assert(!h2_gizclaw_pcm_capture_busy_internal(track));
    assert(h2_gizclaw_pcm_input_prepare(input, NULL) == H2_PAL_ERR_CLOSED);
    uint8_t out[8];
    size_t len;
    assert(h2_gizclaw_pcm_input_read(input, NULL, out, sizeof(out), &len) ==
           H2_PAL_ERR_CLOSED);
    assert(len == 0u);
    state->fail_locks = 1u;
    h2_gizclaw_pcm_input_deinit(input);
    /* Legacy retirement returns its pin without ever needing the Mutex. */
    assert(state->fail_locks == 1u && input->track == NULL &&
           !input->legacy_lease);
    free(input);
    if (destroy_first) {
      assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_IO);
      assert(track != NULL && state->fail_locks == 0u);
    } else {
      h2_gizclaw_pcm_capture_id_t id;
      assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_ERR_IO);
      assert(id == 0u && state->fail_locks == 0u);
      assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
      assert(h2_gizclaw_pcm_capture_discard(track, id) == H2_PAL_OK);
    }
    assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
  }
}

static void capture_validation(const h2_pal_mem_api_t* allocator,
                               const h2_pal_sync_api_t* sync) {
  h2_gizclaw_track_t* track = capture_track(allocator, sync, 8u);
  h2_gizclaw_pcm_capture_id_t id;
  h2_gizclaw_pcm_capture_snapshot_t snapshot;
  h2_gizclaw_pcm_input_t input = {0};
  assert(h2_gizclaw_pcm_capture_begin(NULL, &id) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_capture_begin(track, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_capture_frame_acquire(track, 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_capture_frame_submit(track, 0u, NULL, 0u,
                                             H2_PAL_ERR_TIMEOUT) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_capture_end(track, 0u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_capture_discard(track, 0u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_capture_snapshot(NULL, 0u, &snapshot) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_capture_snapshot(track, 0u, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_input_adopt(&input, track, allocator, 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_capture_begin(track, &id) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_adopt(NULL, track, allocator, id) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_pcm_input_adopt(&input, track, NULL, id) ==
         H2_PAL_ERR_INVALID_ARG);
  sync_state_t* state = sync->user;
  state->fail_lock = true;
  assert(h2_gizclaw_pcm_capture_busy_internal(track));
  assert(h2_gizclaw_pcm_capture_discard(track, id) == H2_PAL_ERR_IO);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_IO);
  state->fail_lock = false;
  assert(h2_gizclaw_pcm_capture_discard(track, id) == H2_PAL_OK);
  state->fail_destroy = true;
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_BUSY);
  assert(track != NULL);
  state->fail_destroy = false;
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

int main(void) {
  uint64_t deadline = 100u;
  assert(h2_gizclaw_audio_next_deadline(deadline, 103u, &deadline) ==
         H2_PAL_OK);
  assert(deadline == 120u); /* 3 ms work, 17 ms remainder. */
  assert(h2_gizclaw_audio_next_deadline(deadline, 126u, &deadline) ==
         H2_PAL_OK);
  assert(deadline == 140u); /* No per-round drift from work time. */
  assert(h2_gizclaw_audio_next_deadline(deadline, 160u, &deadline) ==
         H2_PAL_OK);
  assert(deadline == 160u);
  assert(h2_gizclaw_audio_next_deadline(deadline, 185u, &deadline) ==
         H2_PAL_OK);
  assert(deadline == 185u); /* Overrun: no additional sleep. */
  assert(h2_gizclaw_audio_next_deadline(deadline, 190u, &deadline) ==
         H2_PAL_OK);
  assert(deadline == 205u);
  assert(h2_gizclaw_audio_next_deadline(UINT64_MAX, 0u, &deadline) ==
         H2_PAL_ERR_INVALID_ARG);
  memory_t memory = {0};
  const h2_pal_mem_api_t allocator = {.user = &memory,
                                      .vtable = &memory_vtable};
  boundaries(&allocator);
  concurrency(&allocator);
  counter_wrap(&allocator);
  recording_window(&allocator, NULL);
  default_capacity(&allocator);
  tail_outlives_track(&allocator, NULL);
  discard_downlink(&allocator);
  sync_state_t sync_state = {0};
  const h2_pal_sync_api_t sync = {.user = &sync_state, .vtable = &sync_vtable};
  capture_unsupported(&allocator);
  capture_before_adopt(&allocator, &sync);
  capture_streaming(&allocator, &sync);
  capture_legacy_isolation(&allocator, &sync);
  capture_preserves_legacy_window(&allocator, &sync);
  capture_failures(&allocator, &sync);
  capture_malformed_transient(&allocator, &sync);
  capture_stop_during_frame(&allocator, &sync);
  capture_reader_cleanup(&allocator, &sync);
  capture_reader_deinit_pins_track(&allocator, &sync);
  capture_reader_retirement_retry(&allocator, &sync);
  legacy_reader_retirement_retry(&allocator, &sync);
  capture_validation(&allocator, &sync);
  recording_window(&allocator, &sync);
  tail_outlives_track(&allocator, &sync);
  assert(sync_state.live == 0u);
  assert(memory.live == 0u);
  for (size_t fail = 1u; fail <= 3u; ++fail) {
    memory = (memory_t){.fail_at = fail};
    h2_gizclaw_track_t* track = (void*)1;
    const h2_gizclaw_pcm_track_config_t config = {.allocator = &allocator,
                                                  .uplink_capacity = 32u,
                                                  .downlink_capacity = 32u};
    assert(h2_gizclaw_pcm_track_create(&config, &track) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(track == NULL && memory.live == 0u);
  }
  for (size_t fail = 1u; fail <= 4u; ++fail) {
    memory = (memory_t){.fail_at = fail};
    h2_gizclaw_track_t* track = (void*)1;
    const h2_gizclaw_pcm_track_config_t config = {.allocator = &allocator,
                                                  .uplink_capacity = 8u,
                                                  .downlink_capacity = 8u,
                                                  .sync = &sync};
    assert(h2_gizclaw_pcm_track_create(&config, &track) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(track == NULL && memory.live == 0u && sync_state.live == 0u);
  }
  memory = (memory_t){0};
  h2_gizclaw_track_t* track = (void*)1;
  h2_gizclaw_pcm_track_config_t config = {.allocator = &allocator,
                                          .sync = &sync};
  sync_state.fail_create = true;
  assert(h2_gizclaw_pcm_track_create(&config, &track) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(track == NULL && memory.live == 0u && sync_state.live == 0u);
  sync_state.fail_create = false;
  const h2_pal_sync_api_t invalid_sync = {0};
  config.sync = &invalid_sync;
  assert(h2_gizclaw_pcm_track_create(&config, &track) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(track == NULL && memory.live == 0u);
  return 0;
}
