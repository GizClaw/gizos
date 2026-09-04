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

static void *allocate(void *user, size_t len) {
  memory_t *memory = user;
  if (++memory->attempts == memory->fail_at)
    return NULL;
  void *ptr = malloc(len);
  if (ptr != NULL)
    ++memory->live;
  return ptr;
}

static void release(void *user, void *ptr) {
  memory_t *memory = user;
  if (ptr != NULL) {
    assert(memory->live > 0u);
    --memory->live;
    free(ptr);
  }
}

static const h2_pal_mem_vtable_t memory_vtable = {.alloc = allocate,
                                                  .free = release};

static void boundaries(const h2_pal_mem_api_t *allocator) {
  h2_gizclaw_pcm_track_config_t config = {
      .allocator = allocator, .uplink_capacity = 8u, .downlink_capacity = 16u};
  h2_gizclaw_track_t *track = NULL;
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
    track = (void *)1;
    assert(h2_gizclaw_pcm_track_create(&config, &track) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(track == NULL);
  }
}

typedef struct lane {
  h2_gizclaw_track_t *track;
  bool downlink;
} lane_t;

enum { TRANSFER_BYTES = 42 * 10000 };

static void *producer(void *user) {
  lane_t *lane = user;
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

static void *consumer(void *user) {
  lane_t *lane = user;
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

static void concurrency(const h2_pal_mem_api_t *allocator) {
  const h2_gizclaw_pcm_track_config_t config = {
      .allocator = allocator, .uplink_capacity = 32u, .downlink_capacity = 64u};
  h2_gizclaw_track_t *track = NULL;
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  lane_t lanes[2] = {{track, false}, {track, true}};
  pthread_t threads[4];
  for (size_t i = 0; i < 2u; ++i) {
    assert(pthread_create(&threads[i * 2], NULL, producer, &lanes[i]) == 0);
    assert(pthread_create(&threads[i * 2 + 1], NULL, consumer, &lanes[i]) == 0);
  }
  for (size_t i = 0; i < 4u; ++i)
    assert(pthread_join(threads[i], NULL) == 0);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void counter_wrap(const h2_pal_mem_api_t *allocator) {
  h2_gizclaw_pcm_ring_t ring;
  assert(h2_gizclaw_pcm_ring_init(&ring, allocator, 8u) == H2_PAL_OK);
  atomic_store(&ring.read_index, SIZE_MAX - 3u);
  atomic_store(&ring.write_index, SIZE_MAX - 3u);
  const uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t out[8];
  assert(h2_gizclaw_pcm_ring_write(&ring, data, 8u) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_ring_available(&ring) == 8u);
  assert(h2_gizclaw_pcm_ring_read(&ring, out, 8u) == H2_PAL_OK);
  assert(memcmp(data, out, 8u) == 0);
  assert(h2_gizclaw_pcm_ring_available(&ring) == 0u);
  h2_gizclaw_pcm_ring_deinit(&ring);
}

static void recording_window(const h2_pal_mem_api_t *allocator) {
  const h2_gizclaw_pcm_track_config_t config = {
      .allocator = allocator, .uplink_capacity = 16u, .downlink_capacity = 16u};
  h2_gizclaw_track_t *track = NULL;
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  const uint8_t stale[4] = {1, 2, 3, 4};
  const uint8_t current[10] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
  const uint8_t later[16] = {99};
  h2_gizclaw_pcm_input_t input = {0};
  assert(h2_gizclaw_pcm_input_end(&input, track) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_pcm_track_write(track, stale, sizeof(stale)) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_start(&input, track, allocator) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_start(&input, track, allocator) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_pcm_track_write(track, current, sizeof(current)) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_end(&input, track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_write(track, later, 2u) == H2_PAL_OK);
  /* Neither control call steals the consumer cursor. */
  assert(h2_gizclaw_pcm_track_pending_internal(track) == 16u);
  assert(h2_gizclaw_pcm_input_end(&input, track) == H2_PAL_OK);
  memory_t *memory = allocator->user;
  memory->fail_at = memory->attempts + 1u;
  assert(h2_gizclaw_pcm_input_prepare(&input, track) == H2_PAL_ERR_NO_MEMORY);
  /* Only stale pre-start PCM was discarded. The unsaved tail stays protected. */
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
  assert(h2_gizclaw_pcm_input_read(&input, track, out, 6u, &len) == H2_PAL_OK);
  assert(len == 0u);
  h2_gizclaw_pcm_input_deinit(&input);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void default_capacity(const h2_pal_mem_api_t *allocator) {
  const h2_gizclaw_pcm_track_config_t config = {.allocator = allocator};
  h2_gizclaw_track_t *track = NULL;
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  uint8_t data[16384] = {0};
  assert(h2_gizclaw_pcm_track_write(track, data, sizeof(data)) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_write(track, data, 2u) == H2_PAL_ERR_WOULD_BLOCK);
  assert(track->vtable->write(track->user, data, sizeof(data)) == H2_PAL_OK);
  assert(track->vtable->write(track->user, data, 2u) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_gizclaw_pcm_track_read(track, data, sizeof(data)) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

static void tail_outlives_track(const h2_pal_mem_api_t *service_allocator) {
  memory_t track_memory = {0};
  const h2_pal_mem_api_t track_allocator = {
      .user = &track_memory, .vtable = &memory_vtable};
  const h2_gizclaw_pcm_track_config_t config = {
      .allocator = &track_allocator, .uplink_capacity = 8u, .downlink_capacity = 8u};
  h2_gizclaw_track_t *track = NULL;
  h2_gizclaw_pcm_input_t input = {0};
  const uint8_t data[6] = {0, 1, 2, 3, 4, 5};
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_start(&input, track, service_allocator) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_write(track, data, sizeof(data)) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_end(&input, track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_input_prepare(&input, track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
  assert(track_memory.live == 0u);
  assert(input.tail_len == sizeof(data) && memcmp(input.tail, data, sizeof(data)) == 0);
  h2_gizclaw_pcm_input_deinit(&input);
}

static void discard_downlink(const h2_pal_mem_api_t *allocator) {
  h2_gizclaw_pcm_track_config_t config = {
      .allocator = allocator, .uplink_capacity = 8u, .downlink_capacity = 16u};
  h2_gizclaw_track_t *track = NULL;
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
  recording_window(&allocator);
  default_capacity(&allocator);
  tail_outlives_track(&allocator);
  discard_downlink(&allocator);
  assert(memory.live == 0u);
  for (size_t fail = 1u; fail <= 3u; ++fail) {
    memory = (memory_t){.fail_at = fail};
    h2_gizclaw_track_t *track = (void *)1;
    const h2_gizclaw_pcm_track_config_t config = {.allocator = &allocator,
                                                  .uplink_capacity = 32u,
                                                  .downlink_capacity = 32u};
    assert(h2_gizclaw_pcm_track_create(&config, &track) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(track == NULL && memory.live == 0u);
  }
  return 0;
}
