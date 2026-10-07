#include "driver/flash.h"
#include "driver/flash_partition.h"
#include "flashdb.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_platform_pref_flashdb.c"
#include "os/os.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Real SDK FlashDB/FAL and the production flash port call only these NOR and
 * RTOS boundaries. The image survives independent process launches. */
#define FLASH_SIZE 0x800000u
#define DB_START 0x780000u
#define DB_END 0x7a0000u
static unsigned char flash[FLASH_SIZE];
static const char *image_path;
static unsigned erase_count, write_count;
static uint64_t hardware_reads, hardware_read_bytes;
static uint32_t last_hardware_read_end;
static int fail_manifest_commit;
static uint32_t manifest_commit_status;
static int fail_large_write, fault_nth, fault_reached;
static unsigned hardware_after_fault;
static int erase_fault_mode, read_fault_once;
static flash_protect_type_t protection = FLASH_PROTECT_ALL;
static void persist(void) {
  FILE *f = fopen(image_path, "wb");
  assert(f);
  assert(fwrite(flash, 1, sizeof(flash), f) == sizeof(flash));
  assert(!fclose(f));
}
static void bounds(uint32_t address, uint32_t size) {
  assert(address >= DB_START && address <= DB_END && size <= DB_END - address);
}
bk_err_t bk_flash_read_bytes(uint32_t address, uint8_t *data, uint32_t size) {
  bounds(address, size);
  ++hardware_reads;
  hardware_read_bytes += size;
  last_hardware_read_end = address + size;
  if (read_fault_once) {
    read_fault_once = 0;
    fault_reached = 1;
    return -1;
  }
  memcpy(data, flash + address, size);
  return BK_OK;
}
bk_err_t bk_flash_write_bytes(uint32_t address, const uint8_t *data,
                              uint32_t size) {
  bounds(address, size);
  if (fault_reached)
    ++hardware_after_fault;
  int numbered_fault = fault_nth > 0 && --fault_nth == 0;
  if (getenv("H2_PREF_NOR_TRACE"))
    fprintf(stderr, "NOR write at=%x size=%u nth=%d fail=%d\n", address, size,
            fault_nth, numbered_fault);
  const unsigned char *bytes = data;
  uint32_t count = size;
  int failed =
      numbered_fault ||
      (fail_large_write && address >= H2_BK_PREF_LARGE_OFFSET && size >= 3088u);
  if (failed) {
    fail_large_write = 0;
    fault_reached = 1;
    count = size / 2u;
  }
  for (uint32_t i = 0; i < count; ++i) {
    assert((flash[address + i] & bytes[i]) == bytes[i]);
    flash[address + i] &= bytes[i];
  }
  ++write_count;
  if (fail_manifest_commit && size == LARGE_MANIFEST_SIZE &&
      !memcmp(bytes, "H2LGKV1", 8))
    manifest_commit_status =
        address - 28u - (uint32_t)strlen((const char *)bytes + 8) + 1u;
  if (fail_manifest_commit && manifest_commit_status &&
      address == manifest_commit_status && size == 1u && bytes[0] == 0u) {
    fail_manifest_commit = 0;
    fault_reached = 1;
    return -1; /* physical WRITE flag committed, driver then reports IO */
  }
  return failed ? -1 : BK_OK;
}
bk_err_t bk_flash_erase_sector(uint32_t address) {
  bounds(address, 4096u);
  assert(address % 4096u == 0u);
  if (fault_reached)
    ++hardware_after_fault;
  assert(protection == FLASH_PROTECT_NONE);
  int injected = erase_fault_mode && address >= H2_BK_PREF_LARGE_OFFSET;
  if (injected && erase_fault_mode == 1) {
    erase_fault_mode = 0;
    fault_reached = 1;
    return -1;
  }
  memset(flash + address, 0xff, 4096u);
  ++erase_count;
  if (injected) {
    erase_fault_mode = 0;
    fault_reached = 1;
    return -1;
  }
  return BK_OK;
}
flash_protect_type_t bk_flash_get_protect_type(void) { return protection; }
void bk_flash_set_protect_type(flash_protect_type_t mode) { protection = mode; }
const bk_logic_partition_t *bk_flash_partition_get_info(int id) {
  static const bk_logic_partition_t partition = {DB_START, DB_END - DB_START};
  assert(id == BK_PARTITION_FLASHDB);
  return &partition;
}
int rtos_init_mutex(beken_mutex_t *mutex) {
  *mutex = calloc(1, sizeof(unsigned));
  return *mutex ? 0 : -1;
}
int rtos_deinit_mutex(beken_mutex_t *mutex) {
  free(*mutex);
  *mutex = NULL;
  return 0;
}
int rtos_lock_mutex(beken_mutex_t *mutex) {
  unsigned *p = *mutex;
  assert(p && !*p);
  *p = 1;
  return 0;
}
int rtos_unlock_mutex(beken_mutex_t *mutex) {
  unsigned *p = *mutex;
  assert(p && *p);
  *p = 0;
  return 0;
}
void *os_malloc(size_t size) { return malloc(size); }
void *os_zalloc(size_t size) { return calloc(1, size); }
void *os_realloc(void *p, size_t size) { return realloc(p, size); }
void os_free(void *p) { free(p); }
int easyflash_init(void) { return 0; }
size_t ef_get_env_blob(const char *key, void *data, size_t size,
                       size_t *saved) {
  (void)key;
  (void)data;
  (void)size;
  if (saved)
    *saved = 0;
  return 0;
}
int ef_del_env(const char *key) {
  (void)key;
  return 0;
}
static void *allocate(void *user, size_t size) {
  (void)user;
  return malloc(size);
}
static void release(void *user, void *p) {
  (void)user;
  free(p);
}
static const h2_pal_mem_vtable_t memory_methods = {.alloc = allocate,
                                                   .free = release};
static const h2_pal_mem_api_t memory = {.vtable = &memory_methods};
static unsigned char value[16384], other[16384];
static char text[4096];
static void pattern(unsigned char *bytes, size_t size, unsigned seed) {
  for (size_t i = 0; i < size; ++i)
    bytes[i] = (unsigned char)(i * 17u + (i >> 8) * 31u + seed);
}
static h2_pal_pref_namespace_t *open_ns(const char *name) {
  h2_pal_pref_namespace_t *ns = NULL;
  int rc = h2_pal_pref_open(h2_bk_platform_pref_api(), name,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc)
    fprintf(stderr, "open %s failed rc=%d\n", name, rc);
  assert(!rc && ns);
  return ns;
}
static void expect_blob(h2_pal_pref_namespace_t *ns, const char *key,
                        const void *bytes, size_t size) {
  void *out = NULL;
  size_t length = 0;
  int rc = ns->get_blob(ns, &memory, key, &out, &length);
  if (rc) {
    persist();
    fprintf(stderr, "get %s.%s failed rc=%d\n",
            bk_pref_to_namespace(ns)->name_space, key, rc);
    char full[64];
    assert(!bk_pref_make_key(bk_pref_to_namespace(ns), key, full));
    bk_pref_large_manifest_t manifest;
    int mrc = large_manifest(full, &manifest);
    fprintf(stderr, "manifest rc=%d gen=%llu len=%llu hash=%llx\n", mrc,
            (unsigned long long)manifest.generation,
            (unsigned long long)manifest.length,
            (unsigned long long)manifest.hash);
    if (!mrc)
      for (size_t i = 0; i < (manifest.length + 2999) / 3000; ++i) {
        char chunkkey[64];
        struct fdb_kv item = {0};
        large_chunk_key(full, manifest.generation, i, chunkkey);
        int found =
            fdb_kv_get_obj(&s_pref_large_database, chunkkey, &item) != NULL;
        fprintf(stderr, "chunk %s found=%d len=%lu crc=%d status=%d at=%x\n",
                chunkkey, found, (unsigned long)item.value_len, item.crc_is_ok,
                item.status, item.addr.start);
      }
  }
  assert(!rc && length == size && !memcmp(out, bytes, size));
  free(out);
}
static size_t count(h2_pal_pref_namespace_t *ns) {
  h2_pal_pref_cursor_t *cursor = NULL;
  h2_pal_pref_entry_t entry;
  size_t n = 0;
  int rc;
  while (!(rc = ns->iterate(ns, &cursor, &entry))) {
    assert(entry.key && entry.value_size);
    ++n;
  }
  assert(rc == H2_PAL_ERR_NOT_FOUND);
  assert(!ns->iterate_close(ns, &cursor));
  return n;
}
static void legacy_loader(int seed) {
  /* This handle deliberately uses the old FAL path and 4 KiB geometry, as
   * installed P1 does. It cannot access or format the new tail. */
  struct fdb_kvdb loader = {0};
  struct fdb_blob blob;
  uint32_t old = 0, token = 0x81234567u;
  assert(
      !fdb_kvdb_init(&loader, "old_p1", H2_BK_PREF_FLASHDB_PATH, NULL, NULL));
  assert(loader.parent.sec_size == 4096u && loader.parent.max_size == 24576u);
  if (seed) {
    assert(!fdb_kv_set_blob(&loader, "h2loader.test",
                            fdb_blob_make(&blob, &token, sizeof(token))));
    /* This is the real original capacity failure, before provider routing. */
    assert(fdb_kv_set_blob(&loader, "nor.blob",
                           fdb_blob_make(&blob, value, sizeof(value))) ==
           FDB_SAVED_FULL);
    assert(fdb_kv_set_blob(&loader, "nor.string",
                           fdb_blob_make(&blob, text, 4095u)) ==
           FDB_SAVED_FULL);
  }
  assert(fdb_kv_get_blob(&loader, "h2loader.test",
                         fdb_blob_make(&blob, &old, sizeof(old))) ==
         sizeof(old));
  assert(old == token);
  assert(!fdb_kvdb_deinit(&loader));
}
typedef struct old_entry {
  char key[64];
  size_t length;
  void *data;
} old_entry_t;
static int old_compare(const void *a, const void *b) {
  return strcmp(((const old_entry_t *)a)->key, ((const old_entry_t *)b)->key);
}
static old_entry_t *old_snapshot(size_t *count_out, const char *export_path) {
  struct fdb_kvdb old = {0};
  assert(!fdb_kvdb_init(&old, "old_p1", H2_BK_PREF_FLASHDB_PATH, NULL, NULL));
  old_entry_t *entries = NULL;
  size_t n = 0;
  struct fdb_kv_iterator iterator;
  fdb_kv_iterator_init(&iterator);
  while (fdb_kv_iterate(&old, &iterator)) {
    struct fdb_kv *kv = &iterator.curr_kv;
    if (!strcmp(kv->name, H2_BK_PREF_LARGE_OWNER_KEY))
      continue;
    assert(kv->crc_is_ok && kv->value_len < 4096u);
    entries = realloc(entries, (n + 1u) * sizeof(*entries));
    assert(entries);
    old_entry_t *entry = &entries[n++];
    memcpy(entry->key, kv->name, strlen(kv->name) + 1u);
    entry->length = kv->value_len;
    entry->data = malloc(entry->length ? entry->length : 1u);
    assert(entry->data);
    struct fdb_blob blob;
    assert(fdb_kv_get_blob(&old, entry->key,
                           fdb_blob_make(&blob, entry->data, entry->length)) ==
           entry->length);
  }
  assert(!fdb_kvdb_deinit(&old));
  qsort(entries, n, sizeof(*entries), old_compare);
  if (export_path) {
    FILE *out = fopen(export_path, "wb");
    assert(out);
    for (size_t i = 0; i < n; ++i) {
      uint8_t sizes[16];
      large_put_u64(sizes, strlen(entries[i].key));
      large_put_u64(sizes + 8, entries[i].length);
      assert(fwrite(sizes, 1, sizeof(sizes), out) == sizeof(sizes));
      assert(fwrite(entries[i].key, 1, strlen(entries[i].key), out) ==
             strlen(entries[i].key));
      assert(fwrite(entries[i].data, 1, entries[i].length, out) ==
             entries[i].length);
    }
    assert(!fclose(out));
  }
  *count_out = n;
  return entries;
}
static void old_release(old_entry_t *entries, size_t n) {
  for (size_t i = 0; i < n; ++i)
    free(entries[i].data);
  free(entries);
}
static void old_expect(const old_entry_t *entries, size_t n) {
  size_t actual_count = 0;
  old_entry_t *actual = old_snapshot(&actual_count, NULL);
  assert(actual_count == n);
  for (size_t i = 0; i < n; ++i)
    assert(!strcmp(actual[i].key, entries[i].key) &&
           actual[i].length == entries[i].length &&
           !memcmp(actual[i].data, entries[i].data, entries[i].length));
  old_release(actual, actual_count);
}
static void compatibility(const char *phase, const char *canonical) {
  size_t n = 0;
  old_entry_t *entries = old_snapshot(&n, canonical);
  assert(n > 0);
  h2_pal_pref_namespace_t *probe = open_ns("h2compatprobe");
  old_expect(entries,
             n); /* tail initialization did not change old logical data */
  assert(!probe->close(probe));
  if (!strcmp(phase, "compat")) {
    /* Use a real pre-existing blob/unknown-type key, in a private image copy.
     */
    h2_pal_pref_namespace_t *ns = NULL;
    const char *key = NULL;
    size_t chosen = n;
    for (size_t i = 0; i < n; ++i) {
      const char *dot = strchr(entries[i].key, '.');
      if (!dot || dot == entries[i].key ||
          (size_t)(dot - entries[i].key) > 15u ||
          !strncmp(entries[i].key, "$h2t", 4))
        continue;
      char name[16] = {0};
      memcpy(name, entries[i].key, (size_t)(dot - entries[i].key));
      ns = open_ns(name);
      key = dot + 1;
      void *read = NULL;
      size_t length = 0;
      int rc = ns->get_blob(ns, &memory, key, &read, &length);
      if (rc == H2_PAL_ERR_INVALID_STATE) {
        assert(!ns->close(ns));
        ns = NULL;
        continue;
      }
      assert(!rc && length == entries[i].length &&
             !memcmp(read, entries[i].data, length));
      free(read);
      chosen = i;
      break;
    }
    assert(ns && chosen < n);
    assert(!ns->set_blob(ns, key, value, sizeof(value)));
    assert(!ns->commit(ns));
    void *read = NULL;
    size_t length = 0;
    assert(!ns->get_blob(ns, &memory, key, &read, &length) &&
           length == sizeof(value) && !memcmp(read, value, length));
    free(read);
    /* P1's original raw value is still independently readable. */
    struct fdb_kv item = {0};
    struct fdb_blob blob;
    read = malloc(entries[chosen].length);
    assert(read);
    assert(fdb_kv_get_obj(&s_pref_database, entries[chosen].key, &item));
    assert(
        fdb_kv_get_blob(&s_pref_database, entries[chosen].key,
                        fdb_blob_make(&blob, read, entries[chosen].length)) ==
            entries[chosen].length &&
        !memcmp(read, entries[chosen].data, entries[chosen].length));
    free(read);
    assert(!ns->set_blob(ns, key, value, 1537));
    assert(!ns->commit(ns));
    assert(!ns->get_blob(ns, &memory, key, &read, &length) && length == 1537 &&
           !memcmp(read, value, length));
    free(read);
    assert(!ns->remove(ns, key));
    assert(!ns->commit(ns));
    read = NULL;
    length = 0;
    assert(ns->get_blob(ns, &memory, key, &read, &length) ==
               H2_PAL_ERR_NOT_FOUND &&
           !read && !length);
    assert(!ns->close(ns));
    /* Restore every original raw record (including type sidecars), only
     * within this disposable private NOR image. Canonical bytes must match. */
    for (size_t i = 0; i < n; ++i)
      assert(!fdb_kv_set_blob(
          &s_pref_database, entries[i].key,
          fdb_blob_make(&blob, entries[i].data, entries[i].length)));
    old_expect(entries, n);
  }
  printf("PASS private real-device compatibility old_key_count=%zu\n", n);
  old_release(entries, n);
}

int main(int argc, char **argv) {
  assert(argc == 3 || argc == 4);
  image_path = argv[1];
  const char *phase = argv[2];
  memset(flash, 0xff, sizeof(flash));
  FILE *f = fopen(image_path, "rb");
  if (f) {
    assert(fread(flash, 1, sizeof(flash), f) == sizeof(flash));
    fclose(f);
  }
  assert(!atexit(persist));
  pattern(value, sizeof(value), 5);
  pattern(other, sizeof(other), 77);
  for (unsigned i = 0; i < 4095; ++i)
    text[i] = (char)('a' + i % 23);
  text[4095] = 0;
  if (!strcmp(phase, "compat") || !strcmp(phase, "compat-verify")) {
    assert(argc == 4);
    compatibility(phase, argv[3]);
    return 0;
  }
  if (!strcmp(phase, "unknown-tail")) {
    legacy_loader(1);
    /* Protect previously occupied tail: not merely a zero-filled fixture. */
    flash[H2_BK_PREF_LARGE_OFFSET + 4096] = 0x33;
    unsigned char tail[DB_END - DB_START];
    memcpy(tail, flash + DB_START, sizeof(tail));
    h2_pal_pref_namespace_t *ns = NULL;
    assert(h2_pal_pref_open(h2_bk_platform_pref_api(), "nor",
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns) == H2_PAL_ERR_IO);
    assert(!ns && !memcmp(tail, flash + DB_START, sizeof(tail)));
    puts("PASS unknown occupied tail is not formatted");
    return 0;
  }
  legacy_loader(!strcmp(phase, "seed") || !strcmp(phase, "fault-seed") ||
                !strcmp(phase, "profile") || !strcmp(phase, "cache") ||
                !strcmp(phase, "port-cache") ||
                !strcmp(phase, "remove-prefetch"));
  h2_pal_pref_namespace_t *ns = open_ns("nor"), *isolated = open_ns("norother");
#ifndef H2_PREF_NOR_BASELINE
  if (!strcmp(phase, "port-cache")) {
    const uint32_t sector = DB_END - 4096u, address = sector + 512u;
    uint8_t out[32], changed = 0x7fu;
    h2_bk_pref_flash_read_begin();
    uint64_t start_reads = hardware_reads;
    for (unsigned i = 0; i < 8u; ++i) {
      assert(g_flashdb0.ops.read(address + i * 32u, out, sizeof(out)) ==
             (int)sizeof(out));
      for (size_t j = 0; j < sizeof(out); ++j)
        assert(out[j] == 0xffu);
    }
    assert(hardware_reads == start_reads + 1u);
    assert(g_flashdb0.ops.write(address, &changed, 1u) == 1);
    assert(g_flashdb0.ops.read(address, out, 1u) == 1 && out[0] == changed);
    h2_bk_pref_flash_read_end();
    changed = 0x3fu;
    assert(bk_flash_write_bytes(address, &changed, 1u) == BK_OK);
    h2_bk_pref_flash_read_begin();
    assert(g_flashdb0.ops.read(address, out, 1u) == 1 && out[0] == changed);
    assert(g_flashdb0.ops.erase(sector, 4096u) == 4096);
    assert(g_flashdb0.ops.read(address, out, 1u) == 1 && out[0] == 0xffu);
    assert(g_flashdb0.ops.read(H2_BK_PREF_LARGE_OFFSET - 16u, out, 16u) == 16);
    assert(last_hardware_read_end == H2_BK_PREF_LARGE_OFFSET);
    assert(g_flashdb0.ops.read(DB_END - 8u, out, 8u) == 8);
    assert(last_hardware_read_end == DB_END);
    h2_bk_pref_flash_read_end();
    puts("PASS bounded 512 read-ahead/begin/end/write/erase and both partition "
         "ends");
  } else
#endif
      if (!strcmp(phase, "profile")) {
    assert(!ns->set_blob(ns, "resident", value, sizeof(value)));
    assert(!ns->set_u32(ns, "counter", 1u));
    uint32_t actual = 0;
    assert(!ns->get_u32(ns, "counter", &actual) && actual == 1u);
    uint64_t reads_before = hardware_reads, bytes_before = hardware_read_bytes;
    for (uint32_t i = 0; i < 200u; ++i)
      assert(!ns->set_u32(ns, "counter", i));
    printf("NOR_PROFILE "
           "{\"overwrites\":200,\"hardware_reads\":%llu,\"hardware_bytes\":%"
           "llu}\n",
           (unsigned long long)(hardware_reads - reads_before),
           (unsigned long long)(hardware_read_bytes - bytes_before));
    assert(!ns->get_u32(ns, "counter", &actual) && actual == 199u);
    expect_blob(ns, "resident", value, sizeof(value));
    assert(!ns->clear(ns));
  } else if (!strcmp(phase, "remove-prefetch")) {
    assert(!ns->set_blob(ns, "resident", value, sizeof(value)));
    assert(!isolated->set_blob(isolated, "resident", other, sizeof(other)));
    assert(!ns->set_u32(ns, "pin", 1234u));
    uint64_t reads_before = hardware_reads;
    assert(!ns->remove(ns, "pin"));
    uint64_t reads = hardware_reads - reads_before;
    /* Without iterator prefetch this exact real-engine layout needs 1614
     * hardware reads. Keep a call budget, not a host wall-clock assertion. */
    assert(reads <= 512u);
    uint32_t pin = 99u;
    assert(ns->get_u32(ns, "pin", &pin) == H2_PAL_ERR_NOT_FOUND && pin == 0u);
    expect_blob(ns, "resident", value, sizeof(value));
    expect_blob(isolated, "resident", other, sizeof(other));
    assert(!ns->clear(ns));
    assert(!isolated->clear(isolated));
    printf("PASS real SDK small remove/iterator prefetch reads=%llu\n",
           (unsigned long long)reads);
  } else if (!strcmp(phase, "cache")) {
    void *out = NULL;
    size_t length = 0;
    assert(ns->get_blob(ns, &memory, "route", &out, &length) ==
               H2_PAL_ERR_NOT_FOUND &&
           !out && !length);
    assert(!ns->set_u32(ns, "route", 42u));
    uint32_t number = 0;
    assert(!ns->get_u32(ns, "route", &number) && number == 42u);
    assert(!ns->set_blob(ns, "route", value, sizeof(value)));
    expect_blob(ns, "route", value, sizeof(value));
    assert(!ns->set_blob(ns, "route", other, 1537));
    expect_blob(ns, "route", other, 1537);
    assert(!ns->remove(ns, "route"));
    assert(ns->get_blob(ns, &memory, "route", &out, &length) ==
               H2_PAL_ERR_NOT_FOUND &&
           !out && !length);
    assert(!ns->set_u32(ns, "route", 91u));
    assert(!ns->get_u32(ns, "route", &number) && number == 91u);
    /* Warm a negative route and then fail AFTER the manifest's WRITE flag is
     * physically programmed. Old small raw bytes still exist for P1. */
    fail_manifest_commit = 1;
    assert(ns->set_blob(ns, "route", value, sizeof(value)) == H2_PAL_ERR_IO);
    assert(fault_reached && !fail_manifest_commit && !hardware_after_fault);
    fault_reached = 0;
    expect_blob(ns, "route", value, sizeof(value));
    assert(ns->get_u32(ns, "route", &number) == H2_PAL_ERR_INVALID_STATE);
    assert(!isolated->set_blob(isolated, "route", other, sizeof(other)));
    assert(!ns->clear(ns));
    expect_blob(isolated, "route", other, sizeof(other));
    assert(ns->get_blob(ns, &memory, "route", &out, &length) ==
               H2_PAL_ERR_NOT_FOUND &&
           !out && !length);
    assert(!isolated->clear(isolated));
    puts("PASS cached miss/create/overwrite/remove/clear and "
         "failure-after-manifest-commit route recovery");
  } else if (!strcmp(phase, "seed")) {
    assert(!ns->set_u32(ns, "number", 123));
    assert(!ns->set_blob(ns, "blob", value, sizeof(value)));
    assert(!ns->set_string(ns, "string", text));
    assert(!isolated->set_blob(isolated, "blob", other, sizeof(other)));
    /* Stay in the real tail DB even when a large key shrinks and grows. */
    assert(!ns->set_blob(ns, "blob", value, 1537));
    expect_blob(ns, "blob", value, 1537);
    assert(!ns->set_blob(ns, "blob", value, sizeof(value)));
    unsigned before_gc = erase_count;
    for (unsigned i = 0; i < 80; ++i) {
      pattern(other, sizeof(other), i);
      assert(!isolated->set_blob(isolated, "blob", other, sizeof(other)));
      expect_blob(isolated, "blob", other, sizeof(other));
    }
    assert(erase_count > before_gc); /* actual real engine GC happened */
    for (uint32_t i = 0; i < 1000u; ++i)
      assert(!ns->set_u32(ns, "number", i));
    assert(!ns->commit(ns));
    assert(!isolated->commit(isolated));
    assert(count(ns) == 3 && count(isolated) == 1);
    legacy_loader(0);
    puts("PASS real 4 KiB limit reproduced; 16 KiB/4095/1000/large GC/old P1");
  } else if (!strcmp(phase, "verify")) {
    expect_blob(ns, "blob", value, sizeof(value));
    char *out = NULL;
    assert(!ns->get_string(ns, &memory, "string", &out) && !strcmp(out, text));
    free(out);
    uint32_t number = 0;
    assert(!ns->get_u32(ns, "number", &number) && number == 999u);
    pattern(other, sizeof(other), 79);
    expect_blob(isolated, "blob", other, sizeof(other));
    assert(count(ns) == 3 && count(isolated) == 1);
    puts("PASS fresh process values/type/count/old P1");
  } else if (!strcmp(phase, "clear")) {
    assert(!ns->remove(ns, "blob"));
    assert(!ns->clear(ns));
    assert(!isolated->clear(isolated));
    assert(!ns->commit(ns));
    assert(!isolated->commit(isolated));
    puts("PASS remove/clear+commit");
  } else if (!strcmp(phase, "clean")) {
    void *out = NULL;
    size_t n = 0;
    assert(ns->get_blob(ns, &memory, "blob", &out, &n) ==
               H2_PAL_ERR_NOT_FOUND &&
           !out && !n);
    assert(count(ns) == 0 && count(isolated) == 0);
    puts("PASS fresh process persistent cleanup/repeated completion");
  } else if (!strcmp(phase, "fault-seed")) {
    assert(!ns->set_blob(ns, "blob", value, sizeof(value)));
    assert(!ns->set_string(ns, "string", text));
    assert(!isolated->set_blob(isolated, "blob", other, sizeof(other)));
    for (unsigned i = 0; i < 9; ++i) {
      pattern(value, sizeof(value), i);
      assert(!ns->set_blob(ns, "blob", value, sizeof(value)));
    }
    pattern(value, sizeof(value), 5);
    assert(!ns->set_blob(ns, "blob", value, sizeof(value)));
    assert(!ns->commit(ns));
    puts("PASS fault baseline saved with live peer/GC history");
  } else if (!strcmp(phase, "fault")) {
    fail_large_write = 1;
    assert(ns->set_blob(ns, "blob", other, sizeof(other)) == H2_PAL_ERR_IO);
    assert(!fail_large_write);
    puts("PASS actual NOR short program surfaced IO");
  } else if (!strcmp(phase, "fault-verify")) {
    expect_blob(ns, "blob", value, sizeof(value));
    assert(count(ns) == 2);
    assert(!ns->set_blob(ns, "blob", other, sizeof(other)));
    expect_blob(ns, "blob", other, sizeof(other));
    assert(!ns->clear(ns));
    puts("PASS real engine reboot recovery retained old bytes/type and "
         "retry/cleanup");
  } else if (!strcmp(phase, "fault-read")) {
    read_fault_once = 1;
    void *out = NULL;
    size_t length = 0;
    assert(ns->get_blob(ns, &memory, "blob", &out, &length) == H2_PAL_ERR_IO &&
           !out && !length);
    assert(fault_reached && !read_fault_once && !hardware_after_fault);
    fault_reached = 0;
    expect_blob(ns, "blob", value, sizeof(value));
    expect_blob(isolated, "blob", other, sizeof(other));
    assert(!ns->clear(ns));
    assert(!isolated->clear(isolated));
    puts("PASS read IO returns cleared output then actual recovery/cleanup");
  } else if (!strcmp(phase, "fault-live")) {
    fail_large_write = 1;
    assert(ns->set_blob(ns, "blob", other, sizeof(other)) == H2_PAL_ERR_IO);
    assert(ns->commit(ns) == H2_PAL_ERR_IO);
    assert(!hardware_after_fault);
    fault_reached = 0;
    expect_blob(ns, "blob", value, sizeof(value));
    expect_blob(isolated, "blob", other, sizeof(other));
    assert(!ns->set_blob(ns, "blob", other, sizeof(other)));
    expect_blob(ns, "blob", other, sizeof(other));
    assert(!ns->clear(ns));
    assert(!isolated->clear(isolated));
    puts("PASS same-process real-engine recovery/commit IO/retry/cleanup");
  } else if (!strncmp(phase, "fault-erase-", 12)) {
    erase_fault_mode = atoi(phase + 12);
    assert(erase_fault_mode == 1 || erase_fault_mode == 2);
    int rc = ns->set_blob(ns, "blob", other, sizeof(other));
    assert(fault_reached && !erase_fault_mode && rc == H2_PAL_ERR_IO &&
           !hardware_after_fault);
    puts("PASS erase error before/after physical erase returned IO");
  } else if (!strcmp(phase, "fault-type-verify")) {
    void *out = NULL;
    size_t length = 0;
    int rc = ns->get_blob(ns, &memory, "blob", &out, &length);
    if (!rc) {
      assert(length == sizeof(value) && !memcmp(out, value, length));
      free(out);
    } else {
      assert(rc == H2_PAL_ERR_INVALID_STATE);
      char *string = NULL;
      assert(!ns->get_string(ns, &memory, "blob", &string) &&
             !strcmp(string, text));
      free(string);
    }
    expect_blob(isolated, "blob", other, sizeof(other));
    assert(!ns->clear(ns));
    assert(!isolated->clear(isolated));
    puts("PASS real failure/reboot preserves selected payload and matching "
         "type");
  } else if (!strncmp(phase, "fault-type-", 11)) {
    fault_nth = atoi(phase + 11);
    assert(fault_nth > 0);
    int rc = ns->set_string(ns, "blob", text);
    assert(rc == (fault_reached ? H2_PAL_ERR_IO : H2_PAL_OK));
    assert(!hardware_after_fault);
    printf("PASS numbered type-change fault reached=%d rc=%d\n", fault_reached,
           rc);
  } else if (!strncmp(phase, "fault-nth-", 10)) {
    fault_nth = atoi(phase + 10);
    assert(fault_nth > 0);
    int rc = ns->set_blob(ns, "blob", other, sizeof(other));
    assert(rc == (fault_reached ? H2_PAL_ERR_IO : H2_PAL_OK));
    /* Even SDK GC's ignored writes/erase cannot reach NOR after failure. */
    assert(!hardware_after_fault);
    printf("PASS numbered flash fault reached=%d rc=%d\n", fault_reached, rc);
  } else if (!strcmp(phase, "fault-any-verify")) {
    void *out = NULL;
    size_t n = 0;
    assert(!ns->get_blob(ns, &memory, "blob", &out, &n) && n == sizeof(value));
    assert(!memcmp(out, value, n) || !memcmp(out, other, n));
    free(out);
    expect_blob(isolated, "blob", other, sizeof(other));
    char *out_text = NULL;
    assert(!ns->get_string(ns, &memory, "string", &out_text) &&
           !strcmp(out_text, text));
    free(out_text);
    assert(count(ns) == 2 && count(isolated) == 1);
    assert(!ns->set_blob(ns, "blob", value, sizeof(value)));
    expect_blob(ns, "blob", value, sizeof(value));
    assert(!ns->clear(ns));
    assert(!isolated->clear(isolated));
    puts("PASS real recovery old-or-committed-new/peer/type/retry/clear");
  } else
    assert(0);
  assert(!ns->close(ns));
  assert(!isolated->close(isolated));
  assert(protection == FLASH_PROTECT_ALL);
  printf("NOR phase=%s writes=%u erases=%u\n", phase, write_count, erase_count);
  return 0;
}
