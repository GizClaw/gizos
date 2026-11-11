#include "h2_sctp_internal.h"
#include "h2_sctp_test_peer.h"
#include "h2_test_allocator.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct mutex_fixture {
  h2_pal_result_t create_result, lock_result, destroy_result;
  bool live, locked, missing_handle;
  unsigned locks, unlocks;
  const h2_pal_mem_api_t* mem;
  const h2_pal_crypto_api_t* crypto;
  h2_sctp_registry_entry_t* releasing_entry;
} mutex_fixture_t;

static h2_pal_result_t create_mutex(void* user,
                                    const h2_pal_mutex_config_t* config,
                                    h2_pal_mutex_t** out_mutex) {
  mutex_fixture_t* fixture = user;
  assert(config->flags == H2_PAL_MUTEX_FLAG_NONE && !fixture->live);
  *out_mutex = NULL;
  if (fixture->create_result != H2_PAL_OK) return fixture->create_result;
  if (fixture->missing_handle) return H2_PAL_OK;
  fixture->live = true;
  *out_mutex = (h2_pal_mutex_t*)fixture;
  return H2_PAL_OK;
}

static h2_pal_result_t destroy_mutex(void* user, h2_pal_mutex_t* mutex) {
  mutex_fixture_t* fixture = user;
  assert(mutex == (h2_pal_mutex_t*)fixture && fixture->live &&
         !fixture->locked);
  if (fixture->destroy_result != H2_PAL_OK) return fixture->destroy_result;
  fixture->live = false;
  return H2_PAL_OK;
}

static h2_pal_result_t lock_mutex(void* user, h2_pal_mutex_t* mutex) {
  mutex_fixture_t* fixture = user;
  assert(mutex == (h2_pal_mutex_t*)fixture && fixture->live &&
         !fixture->locked);
  ++fixture->locks;
  if (fixture->lock_result != H2_PAL_OK) return fixture->lock_result;
  fixture->locked = true;
  return H2_PAL_OK;
}

static h2_pal_result_t unlock_mutex(void* user, h2_pal_mutex_t* mutex) {
  mutex_fixture_t* fixture = user;
  assert(mutex == (h2_pal_mutex_t*)fixture && fixture->locked);
  fixture->locked = false;
  ++fixture->unlocks;
  return H2_PAL_OK;
}

static void* unlocked_alloc(void* user, size_t size) {
  mutex_fixture_t* fixture = user;
  assert(!fixture->locked);
  return h2_pal_mem_alloc(fixture->mem, size);
}

static void unlocked_free(void* user, void* pointer) {
  mutex_fixture_t* fixture = user;
  assert(!fixture->locked);
  if (fixture->releasing_entry != NULL)
    assert(!h2_atomic_bool_load(&fixture->releasing_entry->retired,
                                H2_ATOMIC_ACQUIRE));
  h2_pal_mem_free(fixture->mem, pointer);
}

static h2_pal_result_t unlocked_random(void* user, uint8_t* out, size_t len) {
  mutex_fixture_t* fixture = user;
  assert(!fixture->locked);
  return (h2_pal_result_t)h2_pal_crypto_random(fixture->crypto, out, len);
}

static void test_mutex_failures(void) {
  h2_sctp_test_endpoint_t endpoint;
  assert(h2_sctp_test_endpoint_init(&endpoint, H2_PAL_SCTP_ROLE_PASSIVE, 5001u,
                                    5000u, 256u, 1024u, 4096u,
                                    4096u) == H2_PAL_OK);
  const h2_pal_sctp_association_config_t association_config =
      endpoint.association->config;
  h2_sctp_test_endpoint_deinit(&endpoint);
  mutex_fixture_t fixture = {.mem = &endpoint.mem_api,
                             .crypto = &endpoint.crypto_api};
  const h2_pal_sync_vtable_t sync_vtable = {
      .create_mutex = create_mutex,
      .destroy_mutex = destroy_mutex,
      .lock_mutex = lock_mutex,
      .unlock_mutex = unlock_mutex,
  };
  const h2_pal_sync_api_t sync = {&fixture, &sync_vtable};
  const h2_pal_mem_vtable_t mem_vtable = {.alloc = unlocked_alloc,
                                          .free = unlocked_free};
  const h2_pal_mem_api_t mem = {&fixture, &mem_vtable};
  const h2_pal_crypto_vtable_t crypto_vtable = {.random = unlocked_random};
  const h2_pal_crypto_api_t crypto = {&fixture, &crypto_vtable};
  const h2_sctp_config_t config = {.mem = &mem, .crypto = &crypto};
  assert(h2_sctp_create_with_sync(&config, NULL, &endpoint.provider) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(endpoint.provider == NULL);
  const h2_pal_sync_api_t unsupported = {0};
  assert(h2_sctp_create_with_sync(&config, &unsupported, &endpoint.provider) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(endpoint.provider == NULL);
  fixture.create_result = H2_PAL_ERR_IO;
  assert(h2_sctp_create_with_sync(&config, &sync, &endpoint.provider) ==
         H2_PAL_ERR_IO);
  assert(endpoint.provider == NULL && !fixture.live);
  assert(endpoint.allocation_count == endpoint.free_count);
  fixture.create_result = H2_PAL_OK;
  fixture.missing_handle = true;
  assert(h2_sctp_create_with_sync(&config, &sync, &endpoint.provider) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(endpoint.provider == NULL && !fixture.live);
  assert(endpoint.allocation_count == endpoint.free_count);
  fixture.missing_handle = false;
  assert(h2_sctp_create_with_sync(&config, &sync, &endpoint.provider) ==
         H2_PAL_OK);
  endpoint.api = h2_sctp_api(endpoint.provider);
  const size_t live = endpoint.allocation_count - endpoint.free_count;
  fixture.lock_result = H2_PAL_ERR_IO;
  assert(h2_pal_sctp_association_create(endpoint.api, &association_config,
                                        &endpoint.association) ==
         H2_PAL_ERR_IO);
  assert(endpoint.association == NULL && fixture.unlocks == 0u);
  /* The failed publication releases assembly and packet-pool storage too. */
  assert(endpoint.allocation_count - endpoint.free_count == live);
  fixture.lock_result = H2_PAL_OK;
  assert(h2_pal_sctp_association_create(endpoint.api, &association_config,
                                        &endpoint.association) == H2_PAL_OK);
  assert(fixture.unlocks == 1u && !fixture.locked);
  endpoint.reenter_on_state = true;
  assert(h2_pal_sctp_association_start(endpoint.api, endpoint.association,
                                       1u) == H2_PAL_OK);
  assert(endpoint.reentrant_result == H2_PAL_ERR_BUSY);
  assert(endpoint.reentrant_close_result == H2_PAL_ERR_BUSY);
  fixture.releasing_entry = endpoint.association->registry_entry;
  fixture.lock_result = H2_PAL_ERR_IO;
  unsigned callbacks = endpoint.state_events;
  unsigned locks = fixture.locks;
  assert(h2_pal_sctp_association_close(endpoint.api, &endpoint.association) ==
         H2_PAL_OK);
  assert(endpoint.association == NULL && fixture.unlocks == 1u);
  assert(fixture.locks == locks + 1u && endpoint.state_events == callbacks);
  assert(h2_atomic_bool_load(&fixture.releasing_entry->retired,
                             H2_ATOMIC_ACQUIRE));
  fixture.releasing_entry = NULL;
  /* Only the provider and its metadata survive, with no callback borrow. */
  assert(endpoint.allocation_count - endpoint.free_count == live + 1u);
  h2_sctp_t* retained_provider = endpoint.provider;
  fixture.destroy_result = H2_PAL_ERR_IO;
  assert(h2_sctp_destroy(&endpoint.provider) == H2_PAL_ERR_IO);
  assert(endpoint.provider == retained_provider && fixture.live);
  assert(endpoint.provider->registry == NULL);
  assert(endpoint.provider->association_count == 0u);
  assert(endpoint.allocation_count - endpoint.free_count == live);
  fixture.destroy_result = H2_PAL_OK;
  h2_sctp_test_endpoint_deinit(&endpoint);
  assert(!fixture.live && endpoint.provider == NULL);
  assert(endpoint.allocation_count == endpoint.free_count);
  assert(endpoint.packet_allocation_count == endpoint.packet_free_count);
}

/* Callback owners and per-association allocators may die immediately after
 * close, even if registry collection has to wait for another association. */
static void test_close_detaches_borrowed_storage(void) {
  for (unsigned collect = 0u; collect < 4u; ++collect) {
    h2_sctp_test_endpoint_t endpoint;
    assert(h2_sctp_test_endpoint_init(&endpoint, H2_PAL_SCTP_ROLE_PASSIVE,
                                      5001u, 5000u, 256u, 1024u, 4096u,
                                      4096u) == H2_PAL_OK);
    h2_pal_sctp_association_config_t config = endpoint.association->config;
    h2_sctp_test_endpoint_deinit(&endpoint);
    mutex_fixture_t fixture = {0};
    const h2_pal_sync_vtable_t sync_vtable = {
        .create_mutex = create_mutex,
        .destroy_mutex = destroy_mutex,
        .lock_mutex = lock_mutex,
        .unlock_mutex = unlock_mutex,
    };
    const h2_pal_sync_api_t sync = {&fixture, &sync_vtable};
    const h2_sctp_config_t provider_config = {.mem = &endpoint.mem_api,
                                              .crypto = &endpoint.crypto_api};
    assert(h2_sctp_create_with_sync(&provider_config, &sync,
                                    &endpoint.provider) == H2_PAL_OK);
    endpoint.api = h2_sctp_api(endpoint.provider);
    h2_pal_sctp_association_t* survivor = NULL;
    if (collect == 1u || collect == 3u)
      assert(h2_pal_sctp_association_create(endpoint.api, &config, &survivor) ==
             H2_PAL_OK);
    h2_test_allocator_t arena;
    h2_test_allocator_init(&arena);
    h2_sctp_test_endpoint_t* callback_owner =
        h2_pal_mem_alloc(&arena.api, sizeof(*callback_owner));
    assert(callback_owner != NULL);
    memset(callback_owner, 0, sizeof(*callback_owner));
    config.callbacks.user = callback_owner;
    config.allocator = &arena.api;
    /* A registry allocation failure must release every unpublished association
     * allocation too, while leaving the existing provider usable. */
    endpoint.fail_allocation_at = endpoint.allocation_count + 1u;
    assert(h2_pal_sctp_association_create(endpoint.api, &config,
                                          &endpoint.association) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(endpoint.association == NULL && h2_atomic_load(&arena.live) == 1u);
    endpoint.fail_allocation_at = 0u;
    assert(h2_pal_sctp_association_create(endpoint.api, &config,
                                          &endpoint.association) == H2_PAL_OK);
    assert(h2_pal_sctp_association_start(endpoint.api, endpoint.association,
                                         1u) == H2_PAL_OK);
    assert(callback_owner->state_events == 1u);
    assert(h2_atomic_load(&arena.live) > 1u);
    fixture.lock_result = H2_PAL_ERR_IO;
    unsigned locks = fixture.locks;
    unsigned unlocks = fixture.unlocks;
    assert(h2_pal_sctp_association_close(endpoint.api, &endpoint.association) ==
           H2_PAL_OK);
    assert(endpoint.association == NULL);
    assert(fixture.locks == locks + 1u && fixture.unlocks == unlocks);
    assert(h2_atomic_load(&arena.live) == 1u);
    assert(callback_owner->state_events == 1u);
    h2_pal_mem_free(&arena.api, callback_owner);
    assert(h2_atomic_load(&arena.live) == 0u);
    h2_test_allocator_destroy(&arena);
    memset(&arena, 0, sizeof(arena));
    /* From here both the callback context and allocator API are gone. Only
     * metadata allocated by provider_config.mem can remain in the registry. */
    config.allocator = NULL;
    config.callbacks.user = &endpoint;
    if (collect == 0u) {
      fixture.lock_result = H2_PAL_OK;
      assert(h2_pal_sctp_association_create(
                 endpoint.api, &config, &endpoint.association) == H2_PAL_OK);
      assert(endpoint.provider->association_count == 1u);
      assert(h2_pal_sctp_association_close(endpoint.api,
                                           &endpoint.association) == H2_PAL_OK);
    } else if (collect == 1u || collect == 3u) {
      if (collect == 3u) {
        /* Destruction rejects an unrelated live association after collecting
         * the retired entry, without needing the persistently failed lock. */
        assert(h2_sctp_destroy(&endpoint.provider) == H2_PAL_ERR_INVALID_STATE);
        assert(endpoint.provider->association_count == 1u);
      }
      fixture.lock_result = H2_PAL_OK;
      assert(h2_pal_sctp_association_close(endpoint.api, &survivor) ==
             H2_PAL_OK);
      assert(endpoint.provider->registry == NULL);
    }
    h2_sctp_test_endpoint_deinit(&endpoint);
    assert(!fixture.live && endpoint.provider == NULL);
    assert(endpoint.allocation_count ==
           endpoint.free_count + endpoint.allocation_failure_count);
  }
}

int main(void) {
  test_mutex_failures();
  test_close_detaches_borrowed_storage();
  h2_sctp_test_endpoint_t endpoint;
  assert(h2_sctp_test_endpoint_init(&endpoint, H2_PAL_SCTP_ROLE_PASSIVE, 5001u,
                                    5000u, 256u, 1024u, 4096u,
                                    4096u) == H2_PAL_OK);
  const h2_sctp_config_t provider_config = {
      .mem = &endpoint.mem_api,
      .crypto = &endpoint.crypto_api,
  };
  h2_sctp_t* failed_provider = (h2_sctp_t*)(uintptr_t)1u;
  endpoint.fail_allocation_at = endpoint.allocation_count + 1u;
  assert(h2_sctp_create(&provider_config, &failed_provider) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(failed_provider == NULL);
  endpoint.fail_allocation_at = 0u;
  endpoint.random_failure = H2_PAL_ERR_IO;
  h2_pal_sctp_association_t* failed_association =
      (h2_pal_sctp_association_t*)(uintptr_t)1u;
  assert(h2_pal_sctp_association_create(endpoint.api,
                                        &endpoint.association->config,
                                        &failed_association) == H2_PAL_ERR_IO);
  assert(failed_association == NULL);
  endpoint.random_failure = H2_PAL_OK;
  assert(h2_sctp_destroy(&endpoint.provider) == H2_PAL_ERR_INVALID_STATE);
  assert(endpoint.provider != NULL);
  endpoint.reenter_on_state = true;
  assert(h2_pal_sctp_association_start(endpoint.api, endpoint.association,
                                       1u) == H2_PAL_OK);
  assert(endpoint.reentrant_result == H2_PAL_ERR_BUSY);
  assert(endpoint.reentrant_close_result == H2_PAL_ERR_BUSY);
  h2_sctp_test_endpoint_deinit(&endpoint);
  assert(endpoint.allocation_count ==
         endpoint.free_count + endpoint.allocation_failure_count);

  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_pal_sctp_association_start(pair.passive.api,
                                       pair.passive.association,
                                       pair.now_ms) == H2_PAL_OK);
  assert(h2_pal_sctp_association_start(pair.active.api, pair.active.association,
                                       pair.now_ms) == H2_PAL_OK);
  /* An INIT that cannot be answered because every pooled packet buffer is
   * taken leaves the association untouched and the INIT undelivered. */
  uint8_t* held[H2_SCTP_DEFAULT_PACKET_POOL_SIZE];
  for (size_t index = 0u; index < H2_SCTP_DEFAULT_PACKET_POOL_SIZE; ++index) {
    held[index] = h2_sctp_packet_acquire(pair.passive.association);
    assert(held[index] != NULL);
  }
  assert(h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms) == 0u);
  assert(pair.active.packet_head != NULL);
  assert(pair.passive.association->peer_verification_tag == 0u);
  for (size_t index = 0u; index < H2_SCTP_DEFAULT_PACKET_POOL_SIZE; ++index) {
    h2_sctp_packet_release(pair.passive.association, held[index]);
  }
  assert(h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms) == 1u);
  pair.active.fail_allocation_at = pair.active.allocation_count + 1u;
  assert(h2_sctp_test_transfer(&pair.passive, &pair.active, pair.now_ms) == 0u);
  assert(pair.passive.packet_head != NULL);
  assert(pair.active.association->peer_verification_tag == 0u);
  pair.active.fail_allocation_at = 0u;
  (void)h2_sctp_test_pump(&pair, 32u);
  assert(pair.active.state == H2_PAL_SCTP_STATE_CONNECTED);
  assert(pair.passive.state == H2_PAL_SCTP_STATE_CONNECTED);
  h2_sctp_test_pair_deinit(&pair);

  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  pair.active.fail_allocation_at = pair.active.allocation_count + 1u;
  assert(h2_pal_sctp_association_shutdown(pair.active.api,
                                          pair.active.association,
                                          pair.now_ms) == H2_PAL_ERR_NO_MEMORY);
  assert(pair.active.state == H2_PAL_SCTP_STATE_CONNECTED);
  pair.active.fail_allocation_at = 0u;
  const uint32_t reset_sequence = pair.active.association->next_reset_sequence;
  pair.active.fail_allocation_at = pair.active.allocation_count + 2u;
  assert(h2_pal_sctp_association_reset_stream(
             pair.active.api, pair.active.association, 4u, pair.now_ms) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(pair.active.association->next_reset_sequence == reset_sequence);
  assert(!pair.active.association->streams->reset_pending);
  pair.active.fail_allocation_at = 0u;
  const uint8_t data[300] = {0xa5u};
  const h2_pal_sctp_message_t message = {
      .data = data,
      .len = sizeof(data),
      .stream_id = 1u,
      .ppid = 53u,
      .reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE,
  };
  const size_t send_used = pair.active.association->send_used;
  pair.active.fail_allocation_at = pair.active.allocation_count + 2u;
  assert(h2_pal_sctp_association_send_message(
             pair.active.api, pair.active.association, &message, pair.now_ms) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(pair.active.association->send_used == send_used);
  pair.active.fail_allocation_at = 0u;
  pair.active.emit_failure = H2_PAL_ERR_IO;
  assert(h2_pal_sctp_association_send_message(pair.active.api,
                                              pair.active.association, &message,
                                              pair.now_ms) == H2_PAL_ERR_IO);
  assert(pair.active.state == H2_PAL_SCTP_STATE_FAILED);
  assert(pair.active.state_reason == H2_PAL_ERR_IO);
  h2_sctp_test_pair_deinit(&pair);
  assert(pair.active.allocation_count ==
         pair.active.free_count + pair.active.allocation_failure_count);
  assert(pair.passive.allocation_count == pair.passive.free_count);
  return 0;
}
