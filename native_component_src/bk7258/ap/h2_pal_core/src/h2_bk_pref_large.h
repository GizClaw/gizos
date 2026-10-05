#ifndef H2_BK_PREF_LARGE_H
#define H2_BK_PREF_LARGE_H

/* Internal FlashDB-backed large values. Only the new, separately bounded tail
 * uses this encoding. The installed Loader's old KV records are untouched.
 * Write immutable chunks first, then one manifest; reclaim only after publish.
 * FlashDB performs single-record CRC/status/GC/reboot recovery, including the
 * manifest. A failed operation can have committed, as with ordinary FlashDB.
 */
#define LARGE_CHUNK_SIZE 3000u
#define LARGE_VALUE_MAX (32u * 1024u)
#define LARGE_MANIFEST_SIZE 96u
#define LARGE_CHUNK_HEADER_SIZE 88u
/* A namespace is at most 15 bytes. The first dot here is beyond that,
 * so no previously valid public namespace/key can collide with these names. */
#define LARGE_CHUNK_PREFIX "$h2largechunks_v1."

typedef struct bk_pref_large_manifest {
  uint64_t generation, length, hash;
} bk_pref_large_manifest_t;

static uint64_t large_hash(const void *data, size_t size) {
  const uint8_t *bytes = data;
  uint64_t value = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < size; ++i)
    value = (value ^ bytes[i]) * UINT64_C(1099511628211);
  return value;
}
static void large_put_u64(uint8_t *out, uint64_t value) {
  for (unsigned i = 0; i < 8u; ++i)
    out[i] = (uint8_t)(value >> (8u * i));
}
static uint64_t large_get_u64(const uint8_t *bytes) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8u; ++i)
    value |= (uint64_t)bytes[i] << (8u * i);
  return value;
}
static void large_chunk_key(const char *full, uint64_t generation, size_t index,
                            char key[64]) {
  uint64_t hash = large_hash(full, strlen(full));
  (void)snprintf(
      key, 64, LARGE_CHUNK_PREFIX "%08lx%08lx.%08lx%08lx.%04lx",
      (unsigned long)(hash >> 32), (unsigned long)(hash & UINT32_MAX),
      (unsigned long)(generation >> 32),
      (unsigned long)(generation & UINT32_MAX), (unsigned long)index);
}
static int large_manifest(const char *full, bk_pref_large_manifest_t *out) {
  struct fdb_kv item = {0};
  struct fdb_blob blob;
  uint8_t bytes[LARGE_MANIFEST_SIZE] = {0};
  if (!fdb_kv_get_obj(&s_pref_large_database, full, &item))
    return H2_PAL_ERR_NOT_FOUND;
  if (item.value_len != sizeof(bytes) ||
      fdb_kv_get_blob(&s_pref_large_database, full,
                      fdb_blob_make(&blob, bytes, sizeof(bytes))) !=
          sizeof(bytes))
    return H2_PAL_ERR_FORMAT;
  if (memcmp(bytes, "H2LGKV1", 8) ||
      !memchr(bytes + 8, 0, H2_BK_PREF_KEY_MAX) ||
      strcmp((const char *)bytes + 8, full))
    return H2_PAL_ERR_FORMAT;
  out->generation = large_get_u64(bytes + 72);
  out->length = large_get_u64(bytes + 80);
  out->hash = large_get_u64(bytes + 88);
  return out->generation && out->length && out->length <= LARGE_VALUE_MAX
             ? H2_PAL_OK
             : H2_PAL_ERR_FORMAT;
}
static int large_read(const char *full, void *data, size_t capacity,
                      size_t *out_len) {
  bk_pref_large_manifest_t manifest;
  *out_len = 0;
  int rc = large_manifest(full, &manifest);
  if (rc)
    return rc;
  if (manifest.length > capacity)
    return H2_PAL_ERR_NO_SPACE;
  uint8_t *chunk = os_malloc(LARGE_CHUNK_HEADER_SIZE + LARGE_CHUNK_SIZE);
  if (!chunk)
    return H2_PAL_ERR_NO_MEMORY;
  size_t offset = 0;
  for (size_t index = 0; offset < manifest.length; ++index) {
    size_t size = (size_t)manifest.length - offset;
    if (size > LARGE_CHUNK_SIZE)
      size = LARGE_CHUNK_SIZE;
    char key[64];
    struct fdb_blob blob;
    struct fdb_kv item = {0};
    large_chunk_key(full, manifest.generation, index, key);
    memset(chunk, 0, LARGE_CHUNK_HEADER_SIZE);
    if (!fdb_kv_get_obj(&s_pref_large_database, key, &item) ||
        item.value_len != LARGE_CHUNK_HEADER_SIZE + size ||
        fdb_kv_get_blob(
            &s_pref_large_database, key,
            fdb_blob_make(&blob, chunk, LARGE_CHUNK_HEADER_SIZE + size)) !=
            LARGE_CHUNK_HEADER_SIZE + size ||
        memcmp(chunk, "H2LGCH1", 8) ||
        !memchr(chunk + 8, 0, H2_BK_PREF_KEY_MAX) ||
        strcmp((const char *)chunk + 8, full) ||
        large_get_u64(chunk + 72) != manifest.generation ||
        large_get_u64(chunk + 80) != index) {
      rc = H2_PAL_ERR_IO;
      break;
    }
    memcpy((uint8_t *)data + offset, chunk + LARGE_CHUNK_HEADER_SIZE, size);
    offset += size;
  }
  os_free(chunk);
  if (!rc && large_hash(data, offset) != manifest.hash)
    rc = H2_PAL_ERR_IO;
  if (!rc)
    *out_len = offset;
  return rc;
}
/* Delete only owned, unreferenced generations. This also collects partial
 * creation leftovers on remove/clear; a hash collision never deletes a peer.
 * fdb_kv_del only tombstones records and cannot move the iterator's sectors. */
static int large_collect(const char *full,
                         const bk_pref_large_manifest_t *live) {
  struct fdb_kv_iterator iterator;
  fdb_kv_iterator_init(&iterator);
  while (fdb_kv_iterate(&s_pref_large_database, &iterator)) {
    struct fdb_kv *item = &iterator.curr_kv;
    if (strncmp(item->name, LARGE_CHUNK_PREFIX,
                sizeof(LARGE_CHUNK_PREFIX) - 1u))
      continue;
    uint8_t header[LARGE_CHUNK_HEADER_SIZE] = {0};
    struct fdb_blob blob;
    if (item->value_len < LARGE_CHUNK_HEADER_SIZE ||
        item->value_len > LARGE_CHUNK_HEADER_SIZE + LARGE_CHUNK_SIZE ||
        fdb_kv_get_blob(&s_pref_large_database, item->name,
                        fdb_blob_make(&blob, header, sizeof(header))) !=
            sizeof(header) ||
        memcmp(header, "H2LGCH1", 8) ||
        !memchr(header + 8, 0, H2_BK_PREF_KEY_MAX))
      return H2_PAL_ERR_FORMAT;
    if (strcmp((const char *)header + 8, full))
      continue;
    uint64_t generation = large_get_u64(header + 72),
             index = large_get_u64(header + 80);
    char expected[64];
    large_chunk_key(full, generation, (size_t)index, expected);
    if (!generation || index > LARGE_VALUE_MAX / LARGE_CHUNK_SIZE ||
        strcmp(expected, item->name))
      return H2_PAL_ERR_FORMAT;
    if (live && live->generation == generation &&
        index < (live->length + LARGE_CHUNK_SIZE - 1u) / LARGE_CHUNK_SIZE)
      continue;
    int rc = bk_pref_map_flashdb_error(
        fdb_kv_del(&s_pref_large_database, item->name));
    if (rc)
      return rc;
  }
  return H2_PAL_OK;
}
static int large_write(const char *full, const void *data, size_t length) {
  if (length > LARGE_VALUE_MAX)
    return H2_PAL_ERR_NO_SPACE;
  bk_pref_large_manifest_t previous = {0};
  int rc = large_manifest(full, &previous);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND)
    return rc;
  if (previous.generation == UINT64_MAX)
    return H2_PAL_ERR_NO_SPACE;
  bk_pref_large_manifest_t next = {previous.generation + 1u, length,
                                   large_hash(data, length)};
  /* Existing manifest is still authoritative while stale failed chunks are
   * collected, before attempting to allocate another whole generation. */
  rc = large_collect(full, previous.generation ? &previous : NULL);
  if (rc)
    return rc;
  uint8_t *chunk = os_zalloc(LARGE_CHUNK_HEADER_SIZE + LARGE_CHUNK_SIZE);
  if (!chunk)
    return H2_PAL_ERR_NO_MEMORY;
  memcpy(chunk, "H2LGCH1", 8);
  memcpy(chunk + 8, full, strlen(full) + 1u);
  large_put_u64(chunk + 72, next.generation);
  size_t offset = 0;
  for (size_t index = 0; offset < length; ++index) {
    size_t size = length - offset;
    if (size > LARGE_CHUNK_SIZE)
      size = LARGE_CHUNK_SIZE;
    char key[64];
    struct fdb_blob blob;
    struct fdb_kv item = {0};
    large_chunk_key(full, next.generation, index, key);
    /* Immutable names are new. Do not overwrite an unexpected peer even
     * under a digest collision or damaged namespace ownership. */
    if (fdb_kv_get_obj(&s_pref_large_database, key, &item)) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    large_put_u64(chunk + 80, index);
    memcpy(chunk + LARGE_CHUNK_HEADER_SIZE, (const uint8_t *)data + offset,
           size);
    rc = bk_pref_map_flashdb_error(fdb_kv_set_blob(
        &s_pref_large_database, key,
        fdb_blob_make(&blob, chunk, LARGE_CHUNK_HEADER_SIZE + size)));
    if (rc)
      break;
    offset += size;
  }
  os_free(chunk);
  if (rc)
    return rc;
  uint8_t manifest[LARGE_MANIFEST_SIZE] = {0};
  struct fdb_blob blob;
  memcpy(manifest, "H2LGKV1", 8);
  memcpy(manifest + 8, full, strlen(full) + 1u);
  large_put_u64(manifest + 72, next.generation);
  large_put_u64(manifest + 80, next.length);
  large_put_u64(manifest + 88, next.hash);
  rc = bk_pref_map_flashdb_error(
      fdb_kv_set_blob(&s_pref_large_database, full,
                      fdb_blob_make(&blob, manifest, sizeof(manifest))));
  /* On error the old manifest may be PRE_DELETE; leave BOTH generations for
   * the SDK's fresh-init recovery. Never infer an error meant no commit. */
  return rc ? rc : large_collect(full, &next);
}
static int large_remove(const char *full, int *found) {
  struct fdb_kv item = {0};
  if (fdb_kv_get_obj(&s_pref_large_database, full, &item)) {
    int rc =
        bk_pref_map_flashdb_error(fdb_kv_del(&s_pref_large_database, full));
    if (rc)
      return rc;
    *found = 1;
  }
  return large_collect(full, NULL);
}
#endif
