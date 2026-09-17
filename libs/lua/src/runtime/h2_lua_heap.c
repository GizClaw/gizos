#include "h2_lua_internal.h"
#include "h2_tlsf.h"

int h2_lua_heap_size_valid(size_t bytes) {
  const size_t overhead = tlsf_size() + tlsf_pool_overhead();
  return bytes == 0u || (bytes >= overhead + 8u * (tlsf_block_size_min() +
                                                   tlsf_alloc_overhead()) &&
                         bytes - overhead <= tlsf_block_size_max());
}

h2_pal_result_t h2_lua_heap_init(h2_lua_host_t *host) {
  h2_pal_result_t result;
  if (host->config.vm_heap_bytes == 0u) {
    return H2_PAL_OK;
  }
  result = h2_pal_mutex_create(
      host->config.runtime->sync,
      &(h2_pal_mutex_config_t){.name = "h2-lua-heap",
                               .allocator = host->config.runtime->mem},
      &host->vm_heap_mutex);
  if (result != H2_PAL_OK) {
    return result;
  }
  host->vm_heap =
      h2_pal_mem_alloc(host->config.runtime->mem, host->config.vm_heap_bytes);
  if (host->vm_heap == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  /* Validate both control and pool creation: create_with_pool ignores failure
   * from add_pool. Runtime mem supplies ordinary malloc alignment. */
  tlsf_t tlsf = tlsf_create(host->vm_heap);
  if (tlsf == NULL ||
      tlsf_add_pool(tlsf, (char *)host->vm_heap + tlsf_size(),
                    host->config.vm_heap_bytes - tlsf_size()) == NULL) {
    h2_pal_mem_free(host->config.runtime->mem, host->vm_heap);
    host->vm_heap = NULL;
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_OK;
}

void h2_lua_heap_deinit(h2_lua_host_t *host) {
  /* All workers are joined and all VMs closed before this point. */
  if (host->vm_heap != NULL) {
    tlsf_destroy(host->vm_heap);
    h2_pal_mem_free(host->config.runtime->mem, host->vm_heap);
    host->vm_heap = NULL;
  }
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
