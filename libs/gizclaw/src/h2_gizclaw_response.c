#include "h2_gizclaw_response_internal.h"

#include <stdalign.h>
#include <string.h>

static void *arena_alloc(void *user, size_t len) {
  h2_gizclaw_resp_arena_t *arena = user;
  h2_gizclaw_resp_storage_t *storage = arena->storage;
  const size_t alignment = alignof(max_align_t);
  size_t available = storage->capacity - storage->used;
  /* Store the allocation length immediately before an aligned payload. */
  if (available < sizeof(size_t)) {
    arena->exhausted = true;
    return NULL;
  }
  const uintptr_t start =
      (uintptr_t)(storage->data + storage->used + sizeof(size_t));
  const size_t padding = (alignment - start % alignment) % alignment;
  available -= sizeof(size_t);
  if (padding > available || len > available - padding) {
    arena->exhausted = true;
    return NULL;
  }
  uint8_t *data = storage->data + storage->used + sizeof(size_t) + padding;
  memcpy(data - sizeof(size_t), &len, sizeof(len));
  storage->used += sizeof(size_t) + padding + len;
  return data;
}

static void *arena_realloc(void *user, void *ptr, size_t len) {
  if (ptr == NULL)
    return arena_alloc(user, len);
  h2_gizclaw_resp_arena_t *arena = user;
  h2_gizclaw_resp_storage_t *storage = arena->storage;
  uint8_t *data = ptr;
  size_t old_len;
  memcpy(&old_len, data - sizeof(size_t), sizeof(old_len));
  if (len <= old_len)
    return ptr;
  if (data + old_len == storage->data + storage->used &&
      len - old_len <= storage->capacity - storage->used) {
    storage->used += len - old_len;
    memcpy(data - sizeof(size_t), &len, sizeof(len));
    return ptr;
  }
  void *replacement = arena_alloc(user, len);
  if (replacement != NULL)
    memcpy(replacement, ptr, old_len);
  return replacement;
}

static void arena_free(void *user, void *ptr) {
  /* All allocations live in the caller's buffer. Individual frees used by
   * decoder cleanup are harmless; a failed parse rolls back its checkpoint. */
  (void)user;
  (void)ptr;
}

static const h2_pal_mem_vtable_t arena_vtable = {
    .alloc = arena_alloc,
    .realloc = arena_realloc,
    .free = arena_free,
};

h2_pal_result_t h2_gizclaw_resp_arena_begin(h2_gizclaw_resp_storage_t *storage,
                                            h2_gizclaw_resp_arena_t *arena) {
  if (storage == NULL || arena == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  *arena = (h2_gizclaw_resp_arena_t){
      .allocator = {.user = arena, .vtable = &arena_vtable},
      .storage = storage,
      .checkpoint = storage->used,
  };
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_resp_arena_end(h2_gizclaw_resp_arena_t *arena,
                                          h2_pal_result_t result) {
  if (arena->exhausted)
    result = H2_PAL_ERR_NO_SPACE;
  if (result != H2_PAL_OK)
    arena->storage->used = arena->checkpoint;
  return result;
}
