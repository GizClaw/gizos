#include "h2_lua_internal.h"
#include "h2_tlsf.h"

int h2_lua_heap_size_valid(size_t bytes) {
  const size_t overhead = tlsf_size() + tlsf_pool_overhead();
  return bytes == 0u || (bytes >= overhead + 8u * (tlsf_block_size_min() +
                                                   tlsf_alloc_overhead()) &&
                         bytes - overhead <= tlsf_block_size_max());
}

/* A fragmented system heap may not hold the whole reservation in one block.
 * Shrink the request by an eighth per refusal, so each block tracks the
 * largest free region closely, down to this floor; each block becomes a TLSF
 * pool. */
#define H2_LUA_HEAP_CHUNK_MIN_BYTES (256u * 1024u)

static void heap_release_chunks(h2_lua_host_t *host) {
  for (size_t i = 0u; i < host->vm_heap_chunk_count; ++i) {
    h2_pal_mem_free(host->config.runtime->mem, host->vm_heap_chunks[i]);
    host->vm_heap_chunks[i] = NULL;
  }
  host->vm_heap_chunk_count = 0u;
  host->vm_heap_reserved = 0u;
  host->vm_heap = NULL;
}

h2_pal_result_t h2_lua_heap_init(h2_lua_host_t *host) {
  const h2_pal_mem_api_t *mem = host->config.runtime->mem;
  const size_t pool_min = tlsf_pool_overhead() + tlsf_block_size_min() +
                          tlsf_alloc_overhead();
  size_t remaining = host->config.vm_heap_bytes;
  size_t request = remaining;
  h2_pal_result_t result;
  if (remaining == 0u) {
    return H2_PAL_OK;
  }
  result = h2_pal_mutex_create(
      host->config.runtime->sync,
      &(h2_pal_mutex_config_t){.name = "h2-lua-heap", .allocator = mem},
      &host->vm_heap_mutex);
  if (result != H2_PAL_OK) {
    return result;
  }
  while (remaining != 0u) {
    void *chunk;
    size_t pool_offset = 0u;
    if (request > remaining) {
      request = remaining;
    }
    /* Never leave a remainder too small to become a TLSF pool. */
    if (request != remaining && remaining - request < pool_min) {
      request = remaining - pool_min;
    }
    if (host->vm_heap_chunk_count == H2_LUA_HEAP_MAX_CHUNKS ||
        (request < H2_LUA_HEAP_CHUNK_MIN_BYTES && request != remaining)) {
      heap_release_chunks(host);
      return H2_PAL_ERR_NO_MEMORY;
    }
    chunk = h2_pal_mem_alloc(mem, request);
    if (chunk == NULL) {
      /* The last piece may already be below the floor; nothing to halve. */
      if (request == remaining && request < H2_LUA_HEAP_CHUNK_MIN_BYTES) {
        heap_release_chunks(host);
        return H2_PAL_ERR_NO_MEMORY;
      }
      request -= request / 8u;
      continue;
    }
    host->vm_heap_chunks[host->vm_heap_chunk_count++] = chunk;
    if (host->vm_heap == NULL) {
      /* The first block also holds the TLSF control structure. */
      host->vm_heap = tlsf_create(chunk);
      pool_offset = tlsf_size();
    }
    /* add_pool rejects a block TLSF cannot index; treat it as invalid size. */
    if (host->vm_heap == NULL ||
        tlsf_add_pool(host->vm_heap, (char *)chunk + pool_offset,
                      request - pool_offset) == NULL) {
      heap_release_chunks(host);
      return H2_PAL_ERR_INVALID_ARG;
    }
    remaining -= request;
    host->vm_heap_reserved += request;
  }
  return H2_PAL_OK;
}

void h2_lua_heap_deinit(h2_lua_host_t *host) {
  /* All workers are joined and all VMs closed before this point. */
  if (host->vm_heap != NULL) {
    tlsf_destroy(host->vm_heap);
  }
  heap_release_chunks(host);
  if (host->vm_heap_mutex != NULL) {
    (void)h2_pal_mutex_destroy(host->config.runtime->sync, host->vm_heap_mutex);
    host->vm_heap_mutex = NULL;
  }
}

void *h2_lua_heap_realloc(void *user, void *ptr, size_t old_size,
                          size_t new_size) {
  h2_lua_host_t *host = user;
  void *next;
  (void)old_size;
  if (h2_pal_mutex_lock(host->config.runtime->sync, host->vm_heap_mutex) !=
      H2_PAL_OK) {
    return NULL;
  }
  /* TLSF preserves ptr on failure and implements NULL/zero realloc semantics.
   * Release the mutex before Lua can run emergency GC and retry. */
  next = tlsf_realloc(host->vm_heap, ptr, new_size);
  (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->vm_heap_mutex);
  return next;
}
