#include "h2_loader_boot.h"
#include "h2_loader_app_client.h"
#include "h2_loader_status.h"
#include "h2_loader_app_client.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHA_A "abababababababababababababababababababababababababababababababab"
#define SHA_B "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"

typedef struct pref_record {
  const char *key;
  uint8_t data[640];
  size_t len;
  int present;
} pref_record_t;

typedef struct test_fixture {
  h2_pal_pref_namespace_t ns;
  pref_record_t records[4];
  const char *pending_key;
  uint8_t pending_data[640];
  size_t pending_len;
  int pending_remove;
  unsigned set_blob_calls;
  unsigned set_blob_fail_at;
  int set_u32_result;
  int set_i32_result;
  uint32_t boot_intent;
  int boot_intent_present;
  int32_t last_result;
  int last_result_present;
  uint32_t pending_boot_intent;
  int pending_boot_intent_present;
  int32_t pending_last_result;
  int pending_last_result_present;
  uint32_t acceptance_revision;
  int acceptance_revision_present;
  uint32_t pending_acceptance_revision;
  int pending_acceptance_revision_present;
  int commit_result;
  unsigned commit_fail_at;
  unsigned commits;

  uint32_t running_partition;
  uint32_t next_partition;
  int app_partition_bootable;
  unsigned set_next_calls;
  unsigned set_next_fail_at;
  unsigned reboot_calls;
  unsigned prepare_calls;
  unsigned transition_calls;
  int set_next_result;
  int reboot_result;
  int prepare_result;
  int transition_result;

  uint8_t partition_bytes[2][128];
  int reader_result;
  uint32_t writer_partition;
  size_t writer_offset;
  int writer_active;
  int writer_begin_result;
  int writer_write_result;
  int writer_finish_result;
  unsigned writer_aborts;
  uint8_t digest_byte;
  int digest_finish_result;

  int package_present;
  unsigned package_removes;

  h2_pal_pref_api_t pref;
  h2_pal_mem_api_t mem;
  h2_pal_fs_api_t fs;
  h2_pal_power_api_t power;
  h2_loader_image_reader_api_t reader;
  h2_loader_image_writer_api_t writer;
  h2_loader_config_t config;
  h2_loader_t loader;
} test_fixture_t;

static void *test_alloc(void *user, size_t len) {
  (void)user;
  return malloc(len);
}

static void test_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static pref_record_t *find_record(test_fixture_t *fixture, const char *key) {
  for (size_t i = 0u;
       i < sizeof(fixture->records) / sizeof(fixture->records[0]); ++i) {
    if (strcmp(fixture->records[i].key, key) == 0)
      return &fixture->records[i];
  }
  return NULL;
}

static int pref_close(h2_pal_pref_namespace_t *ns) {
  (void)ns;
  return H2_PAL_OK;
}

static int pref_get_blob(h2_pal_pref_namespace_t *ns,
                         const h2_pal_mem_api_t *allocator, const char *key,
                         void **out_data, size_t *out_len) {
  test_fixture_t *fixture = ns->user;
  pref_record_t *record = find_record(fixture, key);
  if (record == NULL || !record->present)
    return H2_PAL_ERR_NOT_FOUND;
  *out_data = h2_pal_mem_alloc(allocator, record->len);
  if (*out_data == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memcpy(*out_data, record->data, record->len);
  *out_len = record->len;
  return H2_PAL_OK;
}

static int pref_set_blob(h2_pal_pref_namespace_t *ns, const char *key,
                         const void *data, size_t len) {
  test_fixture_t *fixture = ns->user;
  ++fixture->set_blob_calls;
  if (fixture->set_blob_fail_at != 0u &&
      fixture->set_blob_calls == fixture->set_blob_fail_at) {
    return H2_PAL_ERR_WRITE;
  }
  assert(find_record(fixture, key) != NULL);
  assert(len <= sizeof(fixture->pending_data));
  fixture->pending_key = key;
  memcpy(fixture->pending_data, data, len);
  fixture->pending_len = len;
  fixture->pending_remove = 0;
  return H2_PAL_OK;
}

static int pref_remove(h2_pal_pref_namespace_t *ns, const char *key) {
  test_fixture_t *fixture = ns->user;
  pref_record_t *record = find_record(fixture, key);
  if (record == NULL)
    return H2_PAL_ERR_NOT_FOUND;
  fixture->pending_key = key;
  fixture->pending_remove = 1;
  return record->present ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
}

static int pref_get_u32(h2_pal_pref_namespace_t *ns, const char *key,
                        uint32_t *out_value) {
  test_fixture_t *fixture = ns->user;
  if (strcmp(key, "mfg_acceptance_revision") == 0) {
    *out_value = 0u;
    if (!fixture->acceptance_revision_present)
      return H2_PAL_ERR_NOT_FOUND;
    *out_value = fixture->acceptance_revision;
    return H2_PAL_OK;
  }
  if (strcmp(key, "boot_intent") != 0 || !fixture->boot_intent_present) {
    *out_value = 0u; /* BK clears outputs even when the key is absent. */
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_value = fixture->boot_intent;
  return H2_PAL_OK;
}

static int pref_set_u32(h2_pal_pref_namespace_t *ns, const char *key,
                        uint32_t value) {
  test_fixture_t *fixture = ns->user;
  assert(strcmp(key, "boot_intent") == 0 ||
         strcmp(key, "mfg_acceptance_revision") == 0);
  if (fixture->set_u32_result != H2_PAL_OK)
    return fixture->set_u32_result;
  if (strcmp(key, "boot_intent") == 0) {
    fixture->pending_boot_intent = value;
    fixture->pending_boot_intent_present = 1;
  } else {
    fixture->pending_acceptance_revision = value;
    fixture->pending_acceptance_revision_present = 1;
  }
  return H2_PAL_OK;
}

static int pref_get_i32(h2_pal_pref_namespace_t *ns, const char *key,
                        int32_t *out_value) {
  test_fixture_t *fixture = ns->user;
  if (strcmp(key, "last_result") != 0 || !fixture->last_result_present) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_value = fixture->last_result;
  return H2_PAL_OK;
}

static int pref_set_i32(h2_pal_pref_namespace_t *ns, const char *key,
                        int32_t value) {
  test_fixture_t *fixture = ns->user;
  assert(strcmp(key, "last_result") == 0);
  if (fixture->set_i32_result != H2_PAL_OK)
    return fixture->set_i32_result;
  fixture->pending_last_result = value;
  fixture->pending_last_result_present = 1;
  return H2_PAL_OK;
}

static void pref_discard_pending(test_fixture_t *fixture) {
  fixture->pending_key = NULL;
  fixture->pending_len = 0u;
  fixture->pending_remove = 0;
  fixture->pending_boot_intent_present = 0;
  fixture->pending_last_result_present = 0;
  fixture->pending_acceptance_revision_present = 0;
}

static int pref_commit(h2_pal_pref_namespace_t *ns) {
  test_fixture_t *fixture = ns->user;
  ++fixture->commits;
  if (fixture->commit_result != H2_PAL_OK ||
      (fixture->commit_fail_at != 0u &&
       fixture->commits == fixture->commit_fail_at)) {
    int result = fixture->commit_result != H2_PAL_OK
                     ? fixture->commit_result
                     : H2_PAL_ERR_WRITE;
    pref_discard_pending(fixture);
    return result;
  }
  if (fixture->pending_key != NULL) {
    pref_record_t *record = find_record(fixture, fixture->pending_key);
    assert(record != NULL);
    if (fixture->pending_remove) {
      record->present = 0;
      record->len = 0u;
    } else {
      memcpy(record->data, fixture->pending_data, fixture->pending_len);
      record->len = fixture->pending_len;
      record->present = 1;
    }
    fixture->pending_key = NULL;
    fixture->pending_remove = 0;
  }
  if (fixture->pending_boot_intent_present) {
    fixture->boot_intent = fixture->pending_boot_intent;
    fixture->boot_intent_present = 1;
  }
  if (fixture->pending_last_result_present) {
    fixture->last_result = fixture->pending_last_result;
    fixture->last_result_present = 1;
  }
  if (fixture->pending_acceptance_revision_present) {
    fixture->acceptance_revision = fixture->pending_acceptance_revision;
    fixture->acceptance_revision_present = 1;
  }
  pref_discard_pending(fixture);
  return H2_PAL_OK;
}

static int pref_open(void *user, const char *name_space,
                     h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t **out_ns) {
  test_fixture_t *fixture = user;
  (void)mode;
  assert(strcmp(name_space, H2_LOADER_PREF_NAMESPACE) == 0);
  fixture->ns = (h2_pal_pref_namespace_t){
      .user = fixture,
      .close = pref_close,
      .get_u32 = pref_get_u32,
      .set_u32 = pref_set_u32,
      .get_i32 = pref_get_i32,
      .set_i32 = pref_set_i32,
      .get_blob = pref_get_blob,
      .set_blob = pref_set_blob,
      .remove = pref_remove,
      .commit = pref_commit,
  };
  *out_ns = &fixture->ns;
  return H2_PAL_OK;
}

static int fs_remove(void *user, const char *path) {
  test_fixture_t *fixture = user;
  assert(strcmp(path, H2_LOADER_DEFAULT_PACKAGE_PATH) == 0 ||
         strcmp(path, "/dl/update.tar.zlib.tmp") == 0 ||
         strcmp(path, "/dl/update.tar.zlib.prev") == 0);
  if (strcmp(path, H2_LOADER_DEFAULT_PACKAGE_PATH) != 0) {
    return H2_PAL_FS_ERR_NOT_FOUND;
  }
  ++fixture->package_removes;
  if (!fixture->package_present)
    return H2_PAL_FS_ERR_NOT_FOUND;
  fixture->package_present = 0;
  return H2_PAL_FS_OK;
}

static h2_pal_result_t power_running(
    void *user,
    h2_pal_power_boot_partition_t *out_partition) {
  test_fixture_t *fixture = user;
  memset(out_partition, 0, sizeof(*out_partition));
  out_partition->id = fixture->running_partition;
  return H2_PAL_OK;
}

static h2_pal_result_t power_next(
    void *user,
    h2_pal_power_boot_partition_t *out_partition) {
  test_fixture_t *fixture = user;
  memset(out_partition, 0, sizeof(*out_partition));
  out_partition->id = fixture->next_partition;
  return H2_PAL_OK;
}

static h2_pal_result_t power_set_next(void *user, uint32_t partition_id) {
  test_fixture_t *fixture = user;
  ++fixture->set_next_calls;
  if (fixture->set_next_result != H2_PAL_OK ||
      (fixture->set_next_fail_at != 0u &&
       fixture->set_next_calls == fixture->set_next_fail_at)) {
    return fixture->set_next_result != H2_PAL_OK
               ? fixture->set_next_result
               : H2_PAL_ERR_IO;
  }
  fixture->next_partition = partition_id;
  return H2_PAL_OK;
}

static h2_pal_result_t power_reboot(void *user, uint32_t reason) {
  test_fixture_t *fixture = user;
  (void)reason;
  ++fixture->reboot_calls;
  return fixture->reboot_result;
}

static int prepare_disruptive(void *user,
                              h2_loader_disruptive_action_t action) {
  test_fixture_t *fixture = user;
  assert(action >= H2_LOADER_DISRUPTIVE_REBOOT_LOADER &&
         action <= H2_LOADER_DISRUPTIVE_REBOOT_APP);
  ++fixture->prepare_calls;
  return fixture->prepare_result;
}

static int reboot_transition(void *user) {
  test_fixture_t *fixture = user;
  ++fixture->transition_calls;
  return fixture->transition_result;
}

static int image_capacity(void *user, uint32_t partition_id, uint64_t *out) {
  (void)user;
  if (partition_id != 1u && partition_id != 2u)
    return H2_PAL_ERR_NOT_FOUND;
  *out = 128u;
  return H2_PAL_OK;
}

static int image_read(void *user, uint32_t partition_id, uint64_t offset,
                      void *data, size_t len) {
  test_fixture_t *fixture = user;
  if (fixture->reader_result != H2_PAL_OK)
    return fixture->reader_result;
  if ((partition_id != 1u && partition_id != 2u) || offset + len > 128u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memcpy(data, fixture->partition_bytes[partition_id - 1u] + offset, len);
  return H2_PAL_OK;
}

static int image_begin(void *user, uint32_t partition_id,
                       const h2_loader_image_identity_t *identity) {
  test_fixture_t *fixture = user;
  (void)identity;
  if (fixture->writer_begin_result != H2_PAL_OK) {
    return fixture->writer_begin_result;
  }
  fixture->writer_partition = partition_id;
  fixture->writer_offset = 0u;
  fixture->writer_active = 1;
  return H2_PAL_OK;
}

static int image_write(void *user, const void *data, size_t len) {
  test_fixture_t *fixture = user;
  if (fixture->writer_write_result != H2_PAL_OK) {
    return fixture->writer_write_result;
  }
  assert(fixture->writer_active);
  memcpy(fixture->partition_bytes[fixture->writer_partition - 1u] +
             fixture->writer_offset,
         data, len);
  fixture->writer_offset += len;
  return H2_PAL_OK;
}

static int image_finish(void *user,
                        const h2_loader_image_identity_t *identity) {
  test_fixture_t *fixture = user;
  (void)identity;
  if (fixture->writer_finish_result != H2_PAL_OK) {
    return fixture->writer_finish_result;
  }
  fixture->writer_active = 0;
  return H2_PAL_OK;
}

static void image_abort(void *user) {
  test_fixture_t *fixture = user;
  fixture->writer_active = 0;
  ++fixture->writer_aborts;
}

static int digest_start(void *user) {
  (void)user;
  return H2_PAL_OK;
}

static int digest_update(void *user, const uint8_t *data, size_t len) {
  (void)user;
  (void)data;
  (void)len;
  return H2_PAL_OK;
}

static int digest_finish(void *user, uint8_t out[32]) {
  test_fixture_t *fixture = user;
  if (fixture->digest_finish_result != H2_PAL_OK)
    return fixture->digest_finish_result;
  memset(out, fixture->digest_byte, 32u);
  return H2_PAL_OK;
}

static void digest_abort(void *user) { (void)user; }

static h2_loader_image_identity_t identity(h2_loader_image_role_t role,
                                           const char *sha) {
  h2_loader_image_identity_t value = {
      .format = 1u,
      .role = role,
      .image_size = 64u,
  };
  (void)snprintf(value.image_sha256, sizeof(value.image_sha256), "%s", sha);
  (void)snprintf(value.version, sizeof(value.version), "%s", "0.2.0");
  (void)snprintf(value.board, sizeof(value.board), "%s", "devkit");
  (void)snprintf(value.target, sizeof(value.target), "%s", "esp32s3");
  return value;
}

static h2_loader_metadata_t metadata(h2_loader_image_role_t role,
                                     const char *sha) {
  h2_loader_metadata_t value = {
      .valid = 1,
      .image_size = 64u,
      .role = role,
  };
  (void)snprintf(value.image_checksum, sizeof(value.image_checksum), "%s", sha);
  (void)snprintf(value.version, sizeof(value.version), "%s", "0.2.0");
  (void)snprintf(value.board, sizeof(value.board), "%s", "devkit");
  (void)snprintf(value.target, sizeof(value.target), "%s", "esp32s3");
  return value;
}

static h2_pal_result_t power_list(void *user,
                                  h2_pal_power_boot_partition_cb_t cb,
                                  void *cb_user) {
  test_fixture_t *fixture = user;
  h2_pal_power_boot_partition_t partition = {
      .id = 1u,
      .flags = H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE,
  };
  h2_pal_result_t rc = cb(cb_user, &partition);
  if (rc != H2_PAL_OK)
    return rc;
  partition.id = 2u;
  partition.flags = fixture->app_partition_bootable
                        ? H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE
                        : H2_PAL_POWER_BOOT_PARTITION_FLAG_NONE;
  return cb(cb_user, &partition);
}

static void fixture_init(test_fixture_t *fixture, uint32_t running_partition) {
  static const h2_pal_pref_vtable_t pref_vtable = {.open = pref_open};
  static const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .free = test_free,
  };
  static const h2_pal_fs_vtable_t fs_vtable = {.remove = fs_remove};
  static const h2_pal_power_vtable_t power_vtable = {
      .list_boot_partitions = power_list,
      .get_running_boot_partition = power_running,
      .get_next_boot_partition = power_next,
      .set_next_boot_partition = power_set_next,
      .reboot = power_reboot,
  };
  static const h2_loader_image_reader_vtable_t reader_vtable = {
      .get_capacity = image_capacity,
      .read = image_read,
  };
  static const h2_loader_image_writer_vtable_t writer_vtable = {
      .get_capacity = image_capacity,
      .begin = image_begin,
      .write = image_write,
      .finish = image_finish,
      .abort = image_abort,
  };
  memset(fixture, 0, sizeof(*fixture));
  fixture->records[0].key = "stage";
  fixture->records[1].key = "partition_1";
  fixture->records[2].key = "partition_2";
  fixture->records[3].key = "mfg";
  fixture->commit_result = H2_PAL_OK;
  fixture->set_u32_result = H2_PAL_OK;
  fixture->set_i32_result = H2_PAL_OK;
  fixture->set_next_result = H2_PAL_OK;
  fixture->reboot_result = H2_PAL_OK;
  fixture->prepare_result = H2_PAL_OK;
  fixture->transition_result = H2_PAL_OK;
  fixture->writer_begin_result = H2_PAL_OK;
  fixture->writer_write_result = H2_PAL_OK;
  fixture->writer_finish_result = H2_PAL_OK;
  fixture->reader_result = H2_PAL_OK;
  fixture->digest_finish_result = H2_PAL_OK;
  fixture->running_partition = running_partition;
  fixture->next_partition = running_partition;
  fixture->app_partition_bootable = 1;
  fixture->digest_byte = 0xabu;
  fixture->pref = (h2_pal_pref_api_t){
      .user = fixture,
      .vtable = &pref_vtable,
  };
  fixture->mem = (h2_pal_mem_api_t){.vtable = &mem_vtable};
  fixture->fs = (h2_pal_fs_api_t){
      .user = fixture,
      .vtable = &fs_vtable,
  };
  fixture->power = (h2_pal_power_api_t){
      .user = fixture,
      .vtable = &power_vtable,
  };
  fixture->reader = (h2_loader_image_reader_api_t){
      .user = fixture,
      .vtable = &reader_vtable,
  };
  fixture->writer = (h2_loader_image_writer_api_t){
      .user = fixture,
      .vtable = &writer_vtable,
  };
  fixture->config = (h2_loader_config_t){
      .package =
          {
              .fs = &fixture->fs,
              .allocator = &fixture->mem,
              .digest =
                  {
                      .user = fixture,
                      .start = digest_start,
                      .update = digest_update,
                      .finish = digest_finish,
                      .abort = digest_abort,
                  },
              .image_reader = &fixture->reader,
              .image_writer = &fixture->writer,
          },
      .pref = &fixture->pref,
      .power = &fixture->power,
      .before_disruptive = prepare_disruptive,
      .disruptive_user = fixture,
      .board = "devkit",
      .target = "esp32s3",
      .chip = "esp32s3",
      .h2loader_partition_id = 1u,
      .app_partition_id = 2u,
      .hardware_capabilities = H2_LOADER_CAPABILITIES_ALL,
      .active_identity = identity(H2_LOADER_IMAGE_ROLE_H2LOADER, SHA_A),
  };
}

static void write_metadata(test_fixture_t *fixture,
                           h2_loader_metadata_slot_t slot,
                           const h2_loader_metadata_t *value) {
  assert(h2_loader_metadata_write(&fixture->pref, slot, value) == H2_PAL_OK);
}

static void fill_max_text(char *value, size_t capacity, char byte) {
  assert(value != NULL && capacity > 1u);
  memset(value, byte, capacity - 1u);
  value[capacity - 1u] = '\0';
}

static void fill_max_status_metadata(h2_loader_metadata_t *metadata,
                                     h2_loader_image_role_t role) {
  memset(metadata, 0, sizeof(*metadata));
  metadata->valid = 1;
  metadata->role = role;
  metadata->package_size = UINT64_MAX;
  metadata->image_size = UINT64_MAX;
  fill_max_text(metadata->package_checksum, sizeof(metadata->package_checksum),
                'a');
  fill_max_text(metadata->image_checksum, sizeof(metadata->image_checksum),
                'b');
  fill_max_text(metadata->version, sizeof(metadata->version), 'v');
  fill_max_text(metadata->board, sizeof(metadata->board), 'b');
  fill_max_text(metadata->target, sizeof(metadata->target), 't');
}

static void test_max_status_fits_public_capacity(void) {
  h2_loader_status_t status;
  char line[H2_LOADER_STATUS_LINE_MAX];
  memset(&status, 0, sizeof(status));
  fill_max_text(status.board, sizeof(status.board), 'b');
  fill_max_text(status.target, sizeof(status.target), 't');
  fill_max_text(status.chip, sizeof(status.chip), 'c');
  fill_max_text(status.device_uid, sizeof(status.device_uid), 'd');
  fill_max_text(status.active_version, sizeof(status.active_version), 'v');
  fill_max_text(status.active_checksum, sizeof(status.active_checksum), 'a');
  status.active_role = H2_LOADER_ACTIVE_ROLE_H2LOADER;
  status.active_image_size = UINT64_MAX;
  status.running_partition_id = UINT32_MAX;
  status.next_partition_id = UINT32_MAX;
  status.boot_intent = H2_LOADER_BOOT_INTENT_AUTO;
  status.capabilities = H2_LOADER_CAPABILITIES_ALL;
  status.command_availability = H2_LOADER_COMMAND_AVAILABILITY_ALL;
  fill_max_status_metadata(&status.stage, H2_LOADER_IMAGE_ROLE_APP);
  fill_max_status_metadata(&status.partition_1, H2_LOADER_IMAGE_ROLE_H2LOADER);
  fill_max_status_metadata(&status.partition_2, H2_LOADER_IMAGE_ROLE_APP);
  status.mfg.total = H2_LOADER_MFG_STEP_MAX;
  memset(status.mfg.step_status, H2_LOADER_MFG_STEP_FAILED,
         sizeof(status.mfg.step_status));
  assert(h2_loader_status_format(&status, line, sizeof(line)) == H2_PAL_OK);
  assert(strlen(line) > 2048u);
  assert(strlen(line) < sizeof(line));
}

static h2_loader_metadata_t read_metadata(test_fixture_t *fixture,
                                          h2_loader_metadata_slot_t slot,
                                          int *present) {
  h2_loader_metadata_t value;
  assert(h2_loader_metadata_read(&fixture->pref, &fixture->mem, slot, &value,
                                 present) == H2_PAL_OK);
  return value;
}

static void test_loader_intent_stays_and_seeds_partition_1(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  int present;
  fixture_init(&fixture, 1u);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
  assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
  assert(fixture.reboot_calls == 0u);
  h2_loader_metadata_t p1 =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &present);
  assert(present && p1.valid && strcmp(p1.image_checksum, SHA_A) == 0);
}

static void test_seed_running_metadata_preserves_package_origin(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  int present;
  h2_loader_metadata_t p1 = metadata(H2_LOADER_IMAGE_ROLE_H2LOADER, SHA_A);
  (void)snprintf(p1.version, sizeof(p1.version), "%s", "stale-version");
  p1.package_size = 1024u;
  (void)snprintf(p1.package_checksum, sizeof(p1.package_checksum), "%s", SHA_B);
  fixture_init(&fixture, 1u);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &p1);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
  p1 = read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &present);
  assert(present && p1.valid);
  assert(strcmp(p1.version, fixture.config.active_identity.version) == 0);
  assert(strcmp(p1.package_checksum, SHA_B) == 0);
  assert(p1.package_size == 1024u);
}

static void test_auto_without_partition_2_stays(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  fixture_init(&fixture, 1u);
  fixture.boot_intent = H2_LOADER_BOOT_INTENT_AUTO;
  fixture.boot_intent_present = 1;
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
  assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
  assert(fixture.reboot_calls == 0u);
}

static void test_auto_boots_valid_different_partition_2(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  h2_loader_metadata_t p2 = metadata(H2_LOADER_IMAGE_ROLE_APP, SHA_B);
  fixture_init(&fixture, 1u);
  fixture.boot_intent = H2_LOADER_BOOT_INTENT_AUTO;
  fixture.boot_intent_present = 1;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &p2);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
  assert(action == H2_LOADER_STARTUP_ACTION_REBOOTING_APP);
  assert(fixture.next_partition == 2u);
  assert(fixture.reboot_calls == 1u);
}

static void test_partition_2_loader_copies_back_without_stage(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  int present;
  fixture_init(&fixture, 2u);
  fixture.last_result = H2_PAL_ERR_WRITE;
  fixture.last_result_present = 1;
  memset(fixture.partition_bytes[1], 0x5a, 64u);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
  assert(action == H2_LOADER_STARTUP_ACTION_REBOOTING_H2LOADER);
  assert(fixture.next_partition == 1u);
  assert(fixture.reboot_calls == 1u);
  h2_loader_metadata_t p1 =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &present);
  assert(present && p1.valid && p1.role == H2_LOADER_IMAGE_ROLE_H2LOADER);
  assert(strcmp(p1.image_checksum, SHA_A) == 0);
  assert(fixture.last_result_present && fixture.last_result == H2_PAL_OK);
}

static void
test_partition_2_loader_preserves_stage_origin_on_both_copies(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  int present;
  h2_loader_metadata_t stage = metadata(H2_LOADER_IMAGE_ROLE_H2LOADER, SHA_A);
  stage.package_size = 1024u;
  (void)snprintf(stage.package_checksum, sizeof(stage.package_checksum), "%s",
                 SHA_B);
  fixture_init(&fixture, 2u);
  fixture.last_result = H2_PAL_ERR_WRITE;
  fixture.last_result_present = 1;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &stage);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
  assert(action == H2_LOADER_STARTUP_ACTION_REBOOTING_H2LOADER);
  h2_loader_metadata_t p1 =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &present);
  assert(present && p1.valid);
  assert(strcmp(p1.package_checksum, SHA_B) == 0 && p1.package_size == 1024u);
  h2_loader_metadata_t p2 =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &present);
  assert(present && p2.valid);
  assert(strcmp(p2.package_checksum, SHA_B) == 0 && p2.package_size == 1024u);
  assert(fixture.last_result_present && fixture.last_result == H2_PAL_OK);
}

static void test_partition_2_copy_failure_keeps_partition_1_invalid(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  int present;
  fixture_init(&fixture, 2u);
  fixture.writer_write_result = H2_PAL_ERR_WRITE;
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_ERR_WRITE);
  assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
  assert(fixture.next_partition == 2u);
  assert(fixture.reboot_calls == 0u);
  h2_loader_metadata_t p1 =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &present);
  assert(present && !p1.valid);
}

static void prepare_partition_2_loader(test_fixture_t *fixture) {
  h2_loader_metadata_t p2 =
      metadata(H2_LOADER_IMAGE_ROLE_H2LOADER, SHA_A);
  fixture_init(fixture, 2u);
  memset(fixture->partition_bytes[1], 0x5a, 64u);
  write_metadata(fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &p2);
}

static void assert_partition_2_retry_converges(test_fixture_t *fixture) {
  h2_loader_startup_action_t action;
  int present;
  fixture->commit_fail_at = 0u;
  fixture->set_blob_fail_at = 0u;
  fixture->set_u32_result = H2_PAL_OK;
  fixture->set_i32_result = H2_PAL_OK;
  fixture->set_next_fail_at = 0u;
  fixture->set_next_result = H2_PAL_OK;
  fixture->writer_begin_result = H2_PAL_OK;
  fixture->writer_write_result = H2_PAL_OK;
  fixture->writer_finish_result = H2_PAL_OK;
  fixture->reader_result = H2_PAL_OK;
  fixture->digest_finish_result = H2_PAL_OK;
  fixture->prepare_result = H2_PAL_OK;
  fixture->reboot_result = H2_PAL_OK;
  fixture->set_next_calls = 0u;
  fixture->running_partition = 2u;
  assert(fixture->next_partition == 2u);
  assert(h2_loader_init(&fixture->loader, &fixture->config) == H2_PAL_OK);
  assert(h2_loader_startup(&fixture->loader, &action) == H2_PAL_OK);
  assert(action == H2_LOADER_STARTUP_ACTION_REBOOTING_H2LOADER);
  assert(fixture->next_partition == 1u);
  h2_loader_metadata_t p1 =
      read_metadata(fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &present);
  assert(present && p1.valid && strcmp(p1.image_checksum, SHA_A) == 0);
}

static void assert_partition_2_failure_is_retryable(test_fixture_t *fixture,
                                                    int expected) {
  h2_loader_startup_action_t action;
  int present = 0;
  assert(h2_loader_init(&fixture->loader, &fixture->config) == H2_PAL_OK);
  assert(h2_loader_startup(&fixture->loader, &action) == expected);
  assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
  assert(fixture->next_partition == 2u);
  h2_loader_metadata_t p1 =
      read_metadata(fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &present);
  assert(!present || !p1.valid ||
         strcmp(p1.image_checksum, SHA_A) == 0);
  assert_partition_2_retry_converges(fixture);
}

static void test_partition_2_copy_recovers_from_every_durable_boundary(void) {
  /* With P2 metadata pre-seeded, copy-back commits P1 invalid, P1 valid,
   * last_result, then boot_intent. Interrupt each transaction and require a
   * reset-style re-entry from P2 to converge safely. */
  for (unsigned commit_offset = 1u; commit_offset <= 4u; ++commit_offset) {
    test_fixture_t fixture;
    prepare_partition_2_loader(&fixture);
    fixture.commit_fail_at = fixture.commits + commit_offset;
    assert_partition_2_failure_is_retryable(&fixture, H2_PAL_ERR_WRITE);
  }

  for (unsigned set_blob_offset = 1u; set_blob_offset <= 2u;
       ++set_blob_offset) {
    test_fixture_t fixture;
    prepare_partition_2_loader(&fixture);
    fixture.set_blob_fail_at = fixture.set_blob_calls + set_blob_offset;
    assert_partition_2_failure_is_retryable(&fixture, H2_PAL_ERR_WRITE);
  }

  test_fixture_t pref_fixture;
  prepare_partition_2_loader(&pref_fixture);
  pref_fixture.set_i32_result = H2_PAL_ERR_WRITE;
  assert_partition_2_failure_is_retryable(&pref_fixture, H2_PAL_ERR_WRITE);

  prepare_partition_2_loader(&pref_fixture);
  pref_fixture.set_u32_result = H2_PAL_ERR_WRITE;
  assert_partition_2_failure_is_retryable(&pref_fixture, H2_PAL_ERR_WRITE);

  for (unsigned writer_step = 0u; writer_step < 3u; ++writer_step) {
    test_fixture_t fixture;
    prepare_partition_2_loader(&fixture);
    if (writer_step == 0u)
      fixture.writer_begin_result = H2_PAL_ERR_WRITE;
    else if (writer_step == 1u)
      fixture.writer_write_result = H2_PAL_ERR_WRITE;
    else
      fixture.writer_finish_result = H2_PAL_ERR_WRITE;
    assert_partition_2_failure_is_retryable(&fixture, H2_PAL_ERR_WRITE);
    if (writer_step != 0u)
      assert(fixture.writer_aborts >= 1u);
  }

  prepare_partition_2_loader(&pref_fixture);
  pref_fixture.reader_result = H2_PAL_ERR_IO;
  assert_partition_2_failure_is_retryable(&pref_fixture, H2_PAL_ERR_IO);

  prepare_partition_2_loader(&pref_fixture);
  pref_fixture.digest_finish_result = H2_PAL_ERR_IO;
  assert_partition_2_failure_is_retryable(&pref_fixture, H2_PAL_ERR_IO);

  for (unsigned set_next_call = 1u; set_next_call <= 2u; ++set_next_call) {
    test_fixture_t fixture;
    prepare_partition_2_loader(&fixture);
    fixture.set_next_fail_at = set_next_call;
    assert_partition_2_failure_is_retryable(&fixture, H2_PAL_ERR_IO);
  }

  test_fixture_t fixture;
  prepare_partition_2_loader(&fixture);
  fixture.prepare_result = H2_PAL_ERR_IO;
  assert_partition_2_failure_is_retryable(&fixture, H2_PAL_ERR_IO);

  prepare_partition_2_loader(&fixture);
  fixture.reboot_result = H2_PAL_ERR_IO;
  assert_partition_2_failure_is_retryable(&fixture, H2_PAL_ERR_IO);
}

static void test_converged_loader_finishes_stage(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  int present;
  h2_loader_metadata_t stage = metadata(H2_LOADER_IMAGE_ROLE_H2LOADER, SHA_A);
  stage.package_size = 1024u;
  (void)snprintf(stage.package_checksum, sizeof(stage.package_checksum), "%s",
                 SHA_B);
  fixture_init(&fixture, 1u);
  fixture.boot_intent = H2_LOADER_BOOT_INTENT_AUTO;
  fixture.boot_intent_present = 1;
  fixture.package_present = 1;
  fixture.last_result = H2_PAL_ERR_WRITE;
  fixture.last_result_present = 1;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &stage);
  h2_loader_metadata_t partition = stage;
  partition.package_checksum[0] = '\0';
  partition.package_size = 0u;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &partition);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &partition);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
  assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
  (void)read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(!present);
  assert(!fixture.package_present && fixture.package_removes == 1u);
  h2_loader_metadata_t p1 =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &present);
  assert(present && strcmp(p1.package_checksum, SHA_B) == 0);
  h2_loader_metadata_t p2 =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &present);
  assert(present && strcmp(p2.package_checksum, SHA_B) == 0);
  assert(fixture.last_result_present && fixture.last_result == H2_PAL_OK);
}

static void test_converged_loader_does_not_ignore_different_stage(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  int present;
  h2_loader_metadata_t loader = metadata(H2_LOADER_IMAGE_ROLE_H2LOADER, SHA_A);
  h2_loader_metadata_t stage = metadata(H2_LOADER_IMAGE_ROLE_APP, SHA_B);
  stage.package_size = 1024u;
  (void)snprintf(stage.package_checksum, sizeof(stage.package_checksum), "%s",
                 SHA_A);
  fixture_init(&fixture, 1u);
  fixture.boot_intent = H2_LOADER_BOOT_INTENT_AUTO;
  fixture.boot_intent_present = 1;
  fixture.package_present = 1;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &stage);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &loader);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &loader);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);

  /* This fixture deliberately lacks package read operations. The important
   * invariant is that a different Stage is inspected instead of being
   * silently skipped merely because P1 and P2 currently match. */
  assert(h2_loader_startup(&fixture.loader, &action) != H2_PAL_OK);
  (void)read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(present && !fixture.loader.status.stage.valid);
}

static void test_same_image_new_package_is_still_inspected(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  int present;
  h2_loader_metadata_t p1 = metadata(H2_LOADER_IMAGE_ROLE_H2LOADER, SHA_A);
  h2_loader_metadata_t p2 = metadata(H2_LOADER_IMAGE_ROLE_APP, SHA_B);
  h2_loader_metadata_t stage = p2;
  p2.package_size = 512u;
  (void)snprintf(p2.package_checksum, sizeof(p2.package_checksum), "%s", SHA_A);
  stage.package_size = 1024u;
  (void)snprintf(stage.package_checksum, sizeof(stage.package_checksum), "%s",
                 SHA_B);
  fixture_init(&fixture, 1u);
  fixture.boot_intent = H2_LOADER_BOOT_INTENT_AUTO;
  fixture.boot_intent_present = 1;
  fixture.package_present = 1;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &stage);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &p1);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &p2);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);

  /* The fixture has no package reader, so reaching inspection is the
   * observable proof that equal image identity did not skip a data-only
   * package update. */
  assert(h2_loader_startup(&fixture.loader, &action) != H2_PAL_OK);
  assert(fixture.reboot_calls == 0u);
  (void)read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(present && !fixture.loader.status.stage.valid);
}

static void test_rolled_back_app_leaves_partition_2_state_untouched(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  int present;
  h2_loader_metadata_t p1 = metadata(H2_LOADER_IMAGE_ROLE_H2LOADER, SHA_A);
  h2_loader_metadata_t stage = metadata(H2_LOADER_IMAGE_ROLE_APP, SHA_B);
  stage.package_size = 1024u;
  (void)snprintf(stage.package_checksum, sizeof(stage.package_checksum), "%s",
                 SHA_A);
  fixture_init(&fixture, 1u);
  fixture.boot_intent = H2_LOADER_BOOT_INTENT_AUTO;
  fixture.boot_intent_present = 1;
  fixture.package_present = 1;
  fixture.app_partition_bootable = 0;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &stage);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &p1);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &stage);
  fixture.last_result = H2_PAL_ERR_WRITE;
  fixture.last_result_present = 1;
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  unsigned commits_before_startup = fixture.commits;

  assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
  assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
  h2_loader_metadata_t retained_stage =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(present && retained_stage.valid);
  assert(h2_loader_metadata_image_equal(&retained_stage, &stage));
  h2_loader_metadata_t p2 =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &present);
  assert(present && p2.valid);
  assert(h2_loader_metadata_image_equal(&p2, &stage));
  assert(fixture.package_present && fixture.package_removes == 0u);
  assert(fixture.boot_intent == H2_LOADER_BOOT_INTENT_AUTO);
  assert(fixture.loader.status.boot_intent == H2_LOADER_BOOT_INTENT_AUTO);
  assert(fixture.last_result_present);
  assert(fixture.last_result == H2_PAL_ERR_WRITE);
  assert(fixture.commits == commits_before_startup);
  assert(fixture.writer_offset == 0u);
  assert(fixture.reboot_calls == 0u);
}

static void test_rolled_back_loader_candidate_is_not_relaunched(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  int present;
  h2_loader_metadata_t p1 = metadata(H2_LOADER_IMAGE_ROLE_H2LOADER, SHA_A);
  h2_loader_metadata_t candidate = metadata(H2_LOADER_IMAGE_ROLE_H2LOADER, SHA_B);
  candidate.package_size = 1024u;
  (void)snprintf(candidate.package_checksum, sizeof(candidate.package_checksum), "%s",
                 SHA_A);
  fixture_init(&fixture, 1u);
  fixture.boot_intent = H2_LOADER_BOOT_INTENT_AUTO;
  fixture.boot_intent_present = 1;
  fixture.package_present = 1;
  /* The platform rolled back an unconfirmed candidate Loader. */
  fixture.app_partition_bootable = 0;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &candidate);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &p1);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &candidate);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);

  assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
  assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
  assert(fixture.reboot_calls == 0u);
  assert(fixture.writer_offset == 0u);
  h2_loader_metadata_t retained_stage =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(present && retained_stage.valid);
  assert(h2_loader_metadata_image_equal(&retained_stage, &candidate));
}

static void test_interrupted_replacement_does_not_boot_failed_app(void) {
  test_fixture_t fixture;
  h2_loader_startup_action_t action;
  h2_loader_metadata_t p1 = metadata(H2_LOADER_IMAGE_ROLE_H2LOADER, SHA_A);
  h2_loader_metadata_t failed = metadata(H2_LOADER_IMAGE_ROLE_APP, SHA_B);
  failed.package_size = 1024u;
  (void)snprintf(failed.package_checksum, sizeof(failed.package_checksum), "%s", SHA_A);
  fixture_init(&fixture, 1u);
  fixture.boot_intent = H2_LOADER_BOOT_INTENT_AUTO;
  fixture.boot_intent_present = 1;
  fixture.app_partition_bootable = 0;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &failed);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_1, &p1);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &failed);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  assert(h2_loader_begin_stage(&fixture.loader, "/dl/update.tar.zlib.tmp",
                               "/dl/update.tar.zlib.prev") == H2_PAL_OK);
  /* A reset after begin but before publish leaves no replacement Stage. */
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  assert(!fixture.loader.status.stage.valid);
  assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
  assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
  assert(fixture.reboot_calls == 0u);
  assert(fixture.writer_offset == 0u);
  assert(h2_loader_metadata_image_equal(&fixture.loader.status.partition_2, &failed));
}

static void test_app_finalize_only_consumes_matching_stage(void) {
  test_fixture_t fixture;
  int present;
  h2_loader_metadata_t stage = metadata(H2_LOADER_IMAGE_ROLE_APP, SHA_A);
  stage.package_size = 1024u;
  (void)snprintf(stage.package_checksum, sizeof(stage.package_checksum), "%s",
                 SHA_B);
  h2_loader_image_identity_t app = identity(H2_LOADER_IMAGE_ROLE_APP, SHA_A);
  fixture_init(&fixture, 2u);
  fixture.package_present = 1;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &stage);
  h2_loader_metadata_t installed;
  assert(h2_loader_metadata_from_stage(&stage, &installed) == H2_PAL_OK);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &installed);
  assert(h2_loader_finalize_active_app(&fixture.pref, &fixture.mem, &fixture.fs,
                                       H2_LOADER_DEFAULT_PACKAGE_PATH, &app, 1u,
                                       2u) == H2_PAL_ERR_INVALID_ARG);
  (void)read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(present && fixture.package_present);
  assert(h2_loader_finalize_active_app(&fixture.pref, &fixture.mem, &fixture.fs,
                                       H2_LOADER_DEFAULT_PACKAGE_PATH, &app, 2u,
                                       2u) == H2_PAL_OK);
  (void)read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(!present && !fixture.package_present);
  h2_loader_metadata_t p2 =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &present);
  assert(present && p2.valid && strcmp(p2.package_checksum, SHA_B) == 0);
  assert(p2.package_size == 1024u);
  assert(fixture.last_result_present && fixture.last_result == H2_PAL_OK);

  /* A newly staged package for the same image is not installed merely by
   * rebooting the already active APP. P2's package source changes only when
   * Loader has actually processed that Stage via reboot upgrade. */
  fixture.package_present = 1;
  stage = p2;
  stage.package_size = 2048u;
  (void)snprintf(stage.package_checksum, sizeof(stage.package_checksum), "%s",
                 SHA_A);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &stage);
  assert(h2_loader_finalize_active_app(&fixture.pref, &fixture.mem, &fixture.fs,
                                       H2_LOADER_DEFAULT_PACKAGE_PATH, &app, 2u,
                                       2u) == H2_PAL_OK);
  (void)read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(present && fixture.package_present);
  p2 = read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &present);
  assert(present && p2.valid && strcmp(p2.package_checksum, SHA_B) == 0);
  assert(p2.package_size == 1024u);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &stage);
  assert(h2_loader_finalize_active_app(&fixture.pref, &fixture.mem, &fixture.fs,
                                       H2_LOADER_DEFAULT_PACKAGE_PATH, &app, 2u,
                                       2u) == H2_PAL_OK);
  (void)read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(!present && !fixture.package_present);

  /* Recovering missing P2 metadata from the running APP must not consume a
   * matching Stage: image identity alone cannot prove that its data payload
   * was installed. */
  fixture.package_present = 1;
  stage = metadata(H2_LOADER_IMAGE_ROLE_APP, SHA_A);
  stage.package_size = 2048u;
  (void)snprintf(stage.package_checksum, sizeof(stage.package_checksum), "%s",
                 SHA_A);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &stage);
  h2_loader_metadata_t invalid = {0};
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &invalid);
  assert(h2_loader_finalize_active_app(&fixture.pref, &fixture.mem, &fixture.fs,
                                       H2_LOADER_DEFAULT_PACKAGE_PATH, &app, 2u,
                                       2u) == H2_PAL_OK);
  p2 = read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &present);
  assert(present && p2.valid && p2.package_checksum[0] == '\0');
  assert(p2.package_size == 0u);
  (void)read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(present && fixture.package_present);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &p2);

  fixture.package_present = 1;
  stage = metadata(H2_LOADER_IMAGE_ROLE_APP, SHA_B);
  stage.package_size = 1024u;
  (void)snprintf(stage.package_checksum, sizeof(stage.package_checksum), "%s",
                 SHA_A);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &stage);
  assert(h2_loader_finalize_active_app(&fixture.pref, &fixture.mem, &fixture.fs,
                                       H2_LOADER_DEFAULT_PACKAGE_PATH, &app, 2u,
                                       2u) == H2_PAL_OK);
  (void)read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(present && fixture.package_present);
  p2 = read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &present);
  assert(present && p2.valid && p2.package_checksum[0] == '\0');
  assert(p2.package_size == 0u);
}

static void test_app_confirmation_is_between_metadata_and_stage_cleanup(void) {
  test_fixture_t fixture;
  int present;
  h2_loader_metadata_t stage = metadata(H2_LOADER_IMAGE_ROLE_APP, SHA_A);
  stage.package_size = 1024u;
  (void)snprintf(stage.package_checksum, sizeof(stage.package_checksum), "%s",
                 SHA_B);
  h2_loader_metadata_t installed;
  assert(h2_loader_metadata_from_stage(&stage, &installed) == H2_PAL_OK);
  h2_loader_image_identity_t app = identity(H2_LOADER_IMAGE_ROLE_APP, SHA_A);

  fixture_init(&fixture, 2u);
  fixture.package_present = 1;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &stage);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &installed);
  fixture.transition_result = H2_PAL_ERR_IO;
  assert(h2_loader_finalize_active_app_with_confirmation(
             &fixture.pref, &fixture.mem, &fixture.fs,
             H2_LOADER_DEFAULT_PACKAGE_PATH, &app, 2u, 2u,
             reboot_transition, &fixture) == H2_PAL_ERR_IO);
  assert(fixture.transition_calls == 1u);
  (void)read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(present && fixture.package_present);
  h2_loader_metadata_t p2 =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &present);
  assert(present && p2.valid && strcmp(p2.package_checksum, SHA_B) == 0);

  fixture.transition_result = H2_PAL_OK;
  assert(h2_loader_finalize_active_app_with_confirmation(
             &fixture.pref, &fixture.mem, &fixture.fs,
             H2_LOADER_DEFAULT_PACKAGE_PATH, &app, 2u, 2u,
             reboot_transition, &fixture) == H2_PAL_OK);
  assert(fixture.transition_calls == 2u);
  (void)read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(!present && !fixture.package_present);

  fixture_init(&fixture, 2u);
  fixture.package_present = 1;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &stage);
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_PARTITION_2, &installed);
  fixture.commit_result = H2_PAL_ERR_WRITE;
  assert(h2_loader_finalize_active_app_with_confirmation(
             &fixture.pref, &fixture.mem, &fixture.fs,
             H2_LOADER_DEFAULT_PACKAGE_PATH, &app, 2u, 2u,
             reboot_transition, &fixture) == H2_PAL_ERR_WRITE);
  assert(fixture.transition_calls == 0u);
  assert(fixture.package_present);
  fixture.commit_result = H2_PAL_OK;
  assert(h2_loader_finalize_active_app_with_confirmation(
             &fixture.pref, &fixture.mem, &fixture.fs,
             H2_LOADER_DEFAULT_PACKAGE_PATH, &app, 2u, 2u,
             reboot_transition, &fixture) == H2_PAL_OK);
  assert(fixture.transition_calls == 1u);
  (void)read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(!present && !fixture.package_present);
}

static void test_reboot_commands_only_set_intent_and_partition(void) {
  test_fixture_t fixture;
  fixture_init(&fixture, 1u);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  fixture.loader.status.partition_2.valid = 1;
  fixture.loader.status.partition_2.role = H2_LOADER_IMAGE_ROLE_APP;
  assert(h2_loader_reboot_app_with_transition(&fixture.loader, NULL, NULL) ==
         H2_PAL_OK);
  assert(fixture.boot_intent == H2_LOADER_BOOT_INTENT_AUTO);
  assert(fixture.next_partition == 2u);
  assert(fixture.reboot_calls == 1u);

  fixture.reboot_calls = 0u;
  assert(h2_loader_reboot_h2loader_with_transition(&fixture.loader, NULL,
                                                   NULL) == H2_PAL_OK);
  assert(fixture.boot_intent == H2_LOADER_BOOT_INTENT_LOADER);
  assert(fixture.next_partition == 1u);
  assert(fixture.reboot_calls == 1u);

  fixture.reboot_calls = 0u;
  assert(h2_loader_reboot_upgrade_with_transition(&fixture.loader, NULL,
                                                  NULL) == H2_PAL_OK);
  assert(fixture.boot_intent == H2_LOADER_BOOT_INTENT_AUTO);
  assert(fixture.next_partition == 1u);
  assert(fixture.reboot_calls == 1u);
  assert(fixture.prepare_calls == 3u);
}

static void test_reboot_preparation_and_failures_do_not_arm_boot(void) {
  test_fixture_t fixture;

  fixture_init(&fixture, 1u);
  fixture.prepare_result = H2_PAL_ERR_IO;
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  fixture.loader.status.partition_2.valid = 1;
  fixture.loader.status.partition_2.role = H2_LOADER_IMAGE_ROLE_APP;
  assert(h2_loader_reboot_app_with_transition(
             &fixture.loader, reboot_transition, &fixture) == H2_PAL_ERR_IO);
  assert(fixture.prepare_calls == 1u);
  assert(fixture.transition_calls == 0u);
  assert(fixture.set_next_calls == 0u);
  assert(fixture.next_partition == 1u);
  assert(fixture.loader.status.boot_intent == H2_LOADER_BOOT_INTENT_LOADER);

  fixture_init(&fixture, 1u);
  fixture.transition_result = H2_PAL_ERR_IO;
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  fixture.loader.status.partition_2.valid = 1;
  fixture.loader.status.partition_2.role = H2_LOADER_IMAGE_ROLE_APP;
  assert(h2_loader_reboot_app_with_transition(
             &fixture.loader, reboot_transition, &fixture) == H2_PAL_ERR_IO);
  assert(fixture.prepare_calls == 1u);
  assert(fixture.transition_calls == 1u);
  assert(fixture.set_next_calls == 2u);
  assert(fixture.next_partition == 1u);
  assert(fixture.boot_intent == H2_LOADER_BOOT_INTENT_LOADER);
  assert(fixture.reboot_calls == 0u);

  fixture_init(&fixture, 1u);
  fixture.reboot_result = H2_PAL_ERR_IO;
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  fixture.loader.status.partition_2.valid = 1;
  fixture.loader.status.partition_2.role = H2_LOADER_IMAGE_ROLE_APP;
  assert(h2_loader_reboot_app_with_transition(
             &fixture.loader, reboot_transition, &fixture) == H2_PAL_ERR_IO);
  assert(fixture.prepare_calls == 1u);
  assert(fixture.transition_calls == 1u);
  assert(fixture.set_next_calls == 2u);
  assert(fixture.next_partition == 1u);
  assert(fixture.boot_intent == H2_LOADER_BOOT_INTENT_LOADER);
  assert(fixture.reboot_calls == 1u);
}

static void test_reboot_app_requires_bootable_partition_and_mfg_gate(void) {
  test_fixture_t fixture;
  fixture_init(&fixture, 1u);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  assert(h2_loader_set_implemented_commands(
             &fixture.loader, H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) ==
         H2_PAL_OK);

  assert((h2_loader_get_command_availability(
              &fixture.loader, &fixture.loader.status) &
          H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) == 0u);
  assert(h2_loader_reboot_app_with_transition(&fixture.loader, NULL, NULL) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(fixture.reboot_calls == 0u);

  fixture.loader.status.partition_2.valid = 1;
  assert((h2_loader_get_command_availability(
              &fixture.loader, &fixture.loader.status) &
          H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) == 0u);
  assert(h2_loader_reboot_app_with_transition(&fixture.loader, NULL, NULL) ==
         H2_PAL_ERR_INVALID_STATE);

  fixture.loader.status.partition_2.role = H2_LOADER_IMAGE_ROLE_APP;
  assert((h2_loader_get_command_availability(
              &fixture.loader, &fixture.loader.status) &
          H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) != 0u);
  fixture.config.mfg_required_total = 1u;
  fixture.loader.config.mfg_required_total = 1u;
  assert((h2_loader_get_command_availability(
              &fixture.loader, &fixture.loader.status) &
          H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) == 0u);
  assert(h2_loader_reboot_app_with_transition(&fixture.loader, NULL, NULL) ==
         H2_PAL_ERR_INVALID_STATE);
}


static void put_test_u32_le(uint8_t *out, uint32_t value) {
  for (size_t i = 0u; i < 4u; ++i)
    out[i] = (uint8_t)(value >> (i * 8u));
}

static void store_mfg_record(test_fixture_t *fixture, const uint8_t *data,
                             size_t len) {
  pref_record_t *record = find_record(fixture, "mfg");
  assert(record != NULL && len <= sizeof(record->data));
  memcpy(record->data, data, len);
  record->len = len;
  record->present = 1;
}

static h2_loader_mfg_summary_t read_mfg(test_fixture_t *fixture,
                                        int *present) {
  h2_loader_mfg_summary_t summary;
  assert(h2_loader_mfg_read(&fixture->pref, &fixture->mem, &summary,
                            present) == H2_PAL_OK);
  return summary;
}

static const char *status_mfg_steps(const h2_loader_mfg_summary_t *summary,
                                    char *line, size_t line_len) {
  h2_loader_status_t status;
  memset(&status, 0, sizeof(status));
  status.active_role = H2_LOADER_ACTIVE_ROLE_H2LOADER;
  status.boot_intent = H2_LOADER_BOOT_INTENT_AUTO;
  assert(h2_loader_status_set_mfg(&status, summary) == H2_PAL_OK);
  assert(h2_loader_status_format(&status, line, line_len) == H2_PAL_OK);
  const char *field = strstr(line, " mfg_mode=");
  assert(field != NULL);
  return field + 1;
}

static void test_mfg_variable_step_round_trip(void) {
  test_fixture_t fixture;
  h2_loader_mfg_summary_t summary;
  int present = 0;
  fixture_init(&fixture, 1u);

  memset(&summary, 0, sizeof(summary));
  summary.total = 24u;
  for (uint32_t i = 0u; i < summary.total; ++i)
    summary.step_status[i] = (uint8_t)(i % 4u);
  assert(h2_loader_mfg_write(&fixture.pref, &summary) == H2_PAL_OK);
  const pref_record_t *record = find_record(&fixture, "mfg");
  assert(record->present && record->len == 5u + 24u);
  assert(record->data[0] == 4u && record->data[1] == 0u &&
         record->data[2] == 0u && record->data[3] == 0u);
  assert(record->data[4] == 24u);
  assert(memcmp(record->data + 5, summary.step_status, 24u) == 0);

  const h2_loader_mfg_summary_t stored = read_mfg(&fixture, &present);
  assert(present == 1);
  assert(memcmp(&stored, &summary, sizeof(summary)) == 0);
  assert(strcmp(h2_loader_mfg_state_name(&stored), "partial") == 0);

  /* Boundary totals and every argument rejection. */
  memset(&summary, 0, sizeof(summary));
  summary.total = 1u;
  summary.step_status[0] = H2_LOADER_MFG_STEP_PASSED;
  assert(h2_loader_mfg_write(&fixture.pref, &summary) == H2_PAL_OK);
  assert(read_mfg(&fixture, &present).total == 1u);
  assert(h2_loader_mfg_summary_is_passed(&summary, 1u));
  assert(!h2_loader_mfg_summary_is_passed(&summary, 2u));
  memset(summary.step_status, H2_LOADER_MFG_STEP_PASSED,
         sizeof(summary.step_status));
  summary.total = H2_LOADER_MFG_STEP_MAX;
  assert(h2_loader_mfg_write(&fixture.pref, &summary) == H2_PAL_OK);
  assert(read_mfg(&fixture, &present).total == H2_LOADER_MFG_STEP_MAX);
  assert(h2_loader_mfg_summary_is_passed(&summary, H2_LOADER_MFG_STEP_MAX));

  const unsigned writes = fixture.set_blob_calls;
  memset(&summary, 0, sizeof(summary));
  assert(h2_loader_mfg_write(&fixture.pref, &summary) ==
         H2_PAL_ERR_INVALID_ARG);
  summary.total = H2_LOADER_MFG_STEP_MAX + 1u;
  assert(h2_loader_mfg_write(&fixture.pref, &summary) ==
         H2_PAL_ERR_INVALID_ARG);
  summary.total = 24u;
  summary.step_status[24] = H2_LOADER_MFG_STEP_PASSED;
  assert(h2_loader_mfg_write(&fixture.pref, &summary) ==
         H2_PAL_ERR_INVALID_ARG);
  summary.step_status[24] = 0u;
  summary.step_status[23] = H2_LOADER_MFG_STEP_FAILED + 1u;
  assert(h2_loader_mfg_write(&fixture.pref, &summary) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_loader_mfg_reset(&fixture.pref, 0u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_loader_mfg_reset(&fixture.pref, H2_LOADER_MFG_STEP_MAX + 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(fixture.set_blob_calls == writes);

  assert(h2_loader_mfg_reset(&fixture.pref, 24u) == H2_PAL_OK);
  const h2_loader_mfg_summary_t reset = read_mfg(&fixture, &present);
  assert(reset.total == 24u);
  for (uint32_t i = 0u; i < H2_LOADER_MFG_STEP_MAX; ++i)
    assert(reset.step_status[i] == H2_LOADER_MFG_STEP_UNTESTED);
}

static void test_mfg_legacy_records_decode_as_22_steps(void) {
  test_fixture_t fixture;
  uint8_t data[64];
  int present = 0;
  fixture_init(&fixture, 1u);

  /* v3: u32 format=3 followed by 22 status bytes. */
  memset(data, 0, sizeof(data));
  put_test_u32_le(data, 3u);
  for (size_t i = 0u; i < 22u; ++i)
    data[4u + i] = (uint8_t)((i * 3u) % 4u);
  store_mfg_record(&fixture, data, 4u + 22u);
  h2_loader_mfg_summary_t summary = read_mfg(&fixture, &present);
  assert(present == 1 && summary.total == 22u);
  assert(memcmp(summary.step_status, data + 4, 22u) == 0);
  for (size_t i = 22u; i < H2_LOADER_MFG_STEP_MAX; ++i)
    assert(summary.step_status[i] == 0u);
  /* The legacy record is rewritten once as v4 with the same content. */
  const pref_record_t *record = find_record(&fixture, "mfg");
  assert(record->len == 5u + 22u && record->data[0] == 4u &&
         record->data[4] == 22u);
  assert(memcmp(record->data + 5, data + 4, 22u) == 0);
  const unsigned writes = fixture.set_blob_calls;
  const h2_loader_mfg_summary_t again = read_mfg(&fixture, &present);
  assert(memcmp(&again, &summary, sizeof(summary)) == 0);
  assert(fixture.set_blob_calls == writes);

  /* v2: counters plus passed/skipped masks. */
  memset(data, 0, sizeof(data));
  put_test_u32_le(data, 2u);
  put_test_u32_le(data + 4, 1u);
  put_test_u32_le(data + 8, 2u);
  put_test_u32_le(data + 12, 22u);
  put_test_u32_le(data + 16, 0x3u);
  put_test_u32_le(data + 20, UINT32_C(1) << 21);
  store_mfg_record(&fixture, data, 24u);
  summary = read_mfg(&fixture, &present);
  assert(summary.total == 22u);
  assert(summary.step_status[0] == H2_LOADER_MFG_STEP_PASSED);
  assert(summary.step_status[1] == H2_LOADER_MFG_STEP_PASSED);
  assert(summary.step_status[2] == H2_LOADER_MFG_STEP_UNTESTED);
  assert(summary.step_status[21] == H2_LOADER_MFG_STEP_SKIPPED);

  /* v1: failed at the first unpassed step. */
  memset(data, 0, sizeof(data));
  put_test_u32_le(data, 1u);
  put_test_u32_le(data + 4, 3u);
  put_test_u32_le(data + 8, 5u);
  put_test_u32_le(data + 12, 22u);
  store_mfg_record(&fixture, data, 16u);
  summary = read_mfg(&fixture, &present);
  assert(summary.total == 22u);
  assert(summary.step_status[4] == H2_LOADER_MFG_STEP_PASSED);
  assert(summary.step_status[5] == H2_LOADER_MFG_STEP_FAILED);

  /* Corrupt v4 records (bad total, length, or status) reset to 22 zeros. */
  static const struct {
    uint8_t total;
    size_t len;
    uint8_t bad_status;
  } corrupt[] = {
      {0u, 5u, 0u},
      {33u, 5u + 33u, 0u},
      {24u, 5u + 23u, 0u},
      {24u, 5u + 25u, 0u},
      {24u, 5u + 24u, 4u},
  };
  for (size_t c = 0u; c < sizeof(corrupt) / sizeof(corrupt[0]); ++c) {
    memset(data, 0, sizeof(data));
    put_test_u32_le(data, 4u);
    data[4] = corrupt[c].total;
    data[5] = corrupt[c].bad_status;
    store_mfg_record(&fixture, data, corrupt[c].len);
    summary = read_mfg(&fixture, &present);
    assert(present == 1 && summary.total == 22u);
    for (size_t i = 0u; i < H2_LOADER_MFG_STEP_MAX; ++i)
      assert(summary.step_status[i] == 0u);
    assert(record->len == 5u + 22u && record->data[4] == 22u);
  }
  /* An unknown format is also reset. */
  memset(data, 0, sizeof(data));
  put_test_u32_le(data, 5u);
  store_mfg_record(&fixture, data, 5u + 24u);
  assert(read_mfg(&fixture, &present).total == 22u);
}

static void test_mfg_acceptance_revision_tracks_total(void) {
  test_fixture_t fixture;
  h2_loader_mfg_summary_t summary;
  uint8_t data[32];
  int present = 0;
  fixture_init(&fixture, 1u);

  assert(h2_loader_mfg_ensure_acceptance_revision(&fixture.pref, 0u, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_loader_mfg_ensure_acceptance_revision(
             &fixture.pref, H2_LOADER_MFG_STEP_MAX + 1u, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_loader_mfg_ensure_acceptance_revision(&fixture.pref, 22u, 0u) ==
         H2_PAL_ERR_INVALID_ARG);

  /* A passed legacy v3 22-step record survives the same revision/total. */
  memset(data, 0, sizeof(data));
  put_test_u32_le(data, 3u);
  memset(data + 4, H2_LOADER_MFG_STEP_PASSED, 22u);
  store_mfg_record(&fixture, data, 4u + 22u);
  fixture.acceptance_revision = 7u;
  fixture.acceptance_revision_present = 1;
  const unsigned writes = fixture.set_blob_calls;
  assert(h2_loader_mfg_ensure_acceptance_revision(&fixture.pref, 22u, 7u) ==
         H2_PAL_OK);
  assert(fixture.set_blob_calls == writes);
  summary = read_mfg(&fixture, &present);
  assert(summary.total == 22u);
  assert(h2_loader_mfg_summary_is_passed(&summary, 22u));

  /* Moving the product from 22 to 24 steps starts clean. */
  assert(h2_loader_mfg_ensure_acceptance_revision(&fixture.pref, 24u, 7u) ==
         H2_PAL_OK);
  summary = read_mfg(&fixture, &present);
  assert(summary.total == 24u);
  for (size_t i = 0u; i < H2_LOADER_MFG_STEP_MAX; ++i)
    assert(summary.step_status[i] == H2_LOADER_MFG_STEP_UNTESTED);
  assert(fixture.acceptance_revision == 7u);

  /* Same revision and total keeps 24-step progress. */
  memset(summary.step_status, H2_LOADER_MFG_STEP_PASSED, 24u);
  assert(h2_loader_mfg_write(&fixture.pref, &summary) == H2_PAL_OK);
  assert(h2_loader_mfg_ensure_acceptance_revision(&fixture.pref, 24u, 7u) ==
         H2_PAL_OK);
  summary = read_mfg(&fixture, &present);
  assert(h2_loader_mfg_summary_is_passed(&summary, 24u));

  /* A revision bump still resets with the requested total. */
  assert(h2_loader_mfg_ensure_acceptance_revision(&fixture.pref, 24u, 8u) ==
         H2_PAL_OK);
  summary = read_mfg(&fixture, &present);
  assert(summary.total == 24u);
  assert(!h2_loader_mfg_summary_is_passed(&summary, 24u));
  assert(fixture.acceptance_revision == 8u);

  /* Missing, corrupt, and oversized records reset despite the revision. */
  find_record(&fixture, "mfg")->present = 0;
  assert(h2_loader_mfg_ensure_acceptance_revision(&fixture.pref, 24u, 8u) ==
         H2_PAL_OK);
  assert(read_mfg(&fixture, &present).total == 24u && present == 1);
  memset(data, 0xffu, sizeof(data));
  store_mfg_record(&fixture, data, 7u);
  assert(h2_loader_mfg_ensure_acceptance_revision(&fixture.pref, 24u, 8u) ==
         H2_PAL_OK);
  assert(read_mfg(&fixture, &present).total == 24u);
  pref_record_t *record = find_record(&fixture, "mfg");
  memset(record->data, 0, 64u);
  record->len = 64u;
  record->present = 1;
  assert(h2_loader_mfg_ensure_acceptance_revision(&fixture.pref, 24u, 8u) ==
         H2_PAL_OK);
  assert(read_mfg(&fixture, &present).total == 24u);
}

static void test_mfg_status_prints_total_steps(void) {
  h2_loader_mfg_summary_t summary;
  char line[H2_LOADER_STATUS_LINE_MAX];

  memset(&summary, 0, sizeof(summary));
  assert(strcmp(status_mfg_steps(&summary, line, sizeof(line)),
                "mfg_mode=1 mfg_steps=0000000000000000000000") == 0);

  summary.total = 22u;
  for (uint32_t i = 0u; i < 22u; ++i)
    summary.step_status[i] = (uint8_t)(i % 4u);
  assert(strcmp(status_mfg_steps(&summary, line, sizeof(line)),
                "mfg_mode=2 mfg_steps=0123012301230123012301") == 0);

  summary.total = 24u;
  summary.step_status[22] = H2_LOADER_MFG_STEP_SKIPPED;
  summary.step_status[23] = H2_LOADER_MFG_STEP_PASSED;
  assert(strcmp(status_mfg_steps(&summary, line, sizeof(line)),
                "mfg_mode=2 mfg_steps=012301230123012301230121") == 0);

  summary.total = 1u;
  memset(summary.step_status, 0, sizeof(summary.step_status));
  summary.step_status[0] = H2_LOADER_MFG_STEP_FAILED;
  assert(strcmp(status_mfg_steps(&summary, line, sizeof(line)),
                "mfg_mode=2 mfg_steps=3") == 0);

  h2_loader_status_t status;
  memset(&status, 0, sizeof(status));
  summary.total = 2u;
  summary.step_status[2] = H2_LOADER_MFG_STEP_PASSED;
  assert(h2_loader_mfg_summary_validate(&summary) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_loader_status_set_mfg(&status, &summary) ==
         H2_PAL_ERR_INVALID_ARG);
  summary.step_status[2] = 0u;
  summary.total = H2_LOADER_MFG_STEP_MAX + 1u;
  assert(h2_loader_mfg_summary_validate(&summary) == H2_PAL_ERR_INVALID_ARG);
}

static void test_mfg_required_total_range_and_gate(void) {
  test_fixture_t fixture;
  h2_loader_mfg_summary_t summary;
  fixture_init(&fixture, 1u);
  fixture.config.mfg_required_total = H2_LOADER_MFG_STEP_MAX + 1u;
  assert(h2_loader_init(&fixture.loader, &fixture.config) ==
         H2_PAL_ERR_INVALID_ARG);

  fixture.config.mfg_required_total = 24u;
  memset(&summary, 0, sizeof(summary));
  summary.total = 24u;
  memset(summary.step_status, H2_LOADER_MFG_STEP_PASSED, 24u);
  assert(h2_loader_mfg_write(&fixture.pref, &summary) == H2_PAL_OK);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  assert(fixture.loader.status.mfg.total == 24u);
  assert(h2_loader_set_implemented_commands(
             &fixture.loader, H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) ==
         H2_PAL_OK);
  fixture.loader.status.partition_2 =
      metadata(H2_LOADER_IMAGE_ROLE_APP, SHA_B);
  assert((h2_loader_get_command_availability(
              &fixture.loader, &fixture.loader.status) &
          H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) != 0u);
  fixture.loader.status.mfg.step_status[23] = H2_LOADER_MFG_STEP_SKIPPED;
  assert((h2_loader_get_command_availability(
              &fixture.loader, &fixture.loader.status) &
          H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) == 0u);
}

static void
test_stage_begin_invalidates_metadata_and_removes_old_package(void) {
  test_fixture_t fixture;
  fixture_init(&fixture, 1u);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
  h2_loader_metadata_t stage = metadata(H2_LOADER_IMAGE_ROLE_APP, SHA_B);
  (void)snprintf(stage.package_checksum, sizeof(stage.package_checksum), "%s",
                 SHA_B);
  stage.package_size = 64u;
  write_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &stage);
  fixture.loader.status.stage = stage;
  fixture.package_present = 1;

  assert(h2_loader_begin_stage(&fixture.loader, "/dl/update.tar.zlib.tmp",
                               "/dl/update.tar.zlib.prev") == H2_PAL_OK);
  assert(fixture.package_present == 0);
  assert(fixture.package_removes == 1u);
  int present = 0;
  const h2_loader_metadata_t stored =
      read_metadata(&fixture, H2_LOADER_METADATA_SLOT_STAGE, &present);
  assert(present == 1);
  assert(stored.valid == 0);
  assert(fixture.loader.status.stage.valid == 0);
}

static uint64_t app_test_now(void *user) { (void)user; return 0u; }
static void app_test_sleep(void *user, uint32_t ms) { (void)user; (void)ms; }

static void test_app_client_package_entry_matches_loader(void) {
  test_fixture_t fixture;
  fixture_init(&fixture, 2u);
  const h2_pal_disk_api_t disk = {0};
  const h2_pal_http_api_t http = {0};
  const h2_pal_wifi_sta_api_t wifi = {0};
  h2_loader_app_client_t client;
  h2_loader_app_client_config_t config = {
      .pref = &fixture.pref, .power = &fixture.power,
      .allocator = &fixture.mem, .fs = &fixture.fs,
      .disk = &disk, .http = &http, .wifi = &wifi,
      .digest = fixture.config.package.digest,
      .board = fixture.config.board, .target = fixture.config.target,
      .chip = fixture.config.chip,
      .active_identity = identity(H2_LOADER_IMAGE_ROLE_APP, SHA_B),
      .hardware_capabilities = fixture.config.hardware_capabilities,
      .h2loader_partition_id = 1u, .app_partition_id = 2u,
      .now_ms = app_test_now, .sleep_ms = app_test_sleep,
  };
  assert(h2_loader_app_client_init(&client, &config) == H2_PAL_OK);
  assert(strcmp(client.loader.package.config.app_entry_path,
                H2_LOADER_DEFAULT_APP_ENTRY_PATH) == 0);
  config.app_entry_path = "app/jieli/update.ufw";
  assert(h2_loader_app_client_init(&client, &config) == H2_PAL_OK);
  assert(strcmp(client.loader.config.package.app_entry_path,
                config.app_entry_path) == 0);
  assert(strcmp(client.loader.package.config.app_entry_path,
                config.app_entry_path) == 0);
}

static void test_empty_pref_preserves_default_boot_intent(void) {
  test_fixture_t fixture;
  h2_loader_status_t status;
  fixture_init(&fixture, 1u);
  fixture.boot_intent_present = 0;
  fixture.last_result_present = 0;
  assert(h2_loader_read_pref_status(&fixture.pref, &fixture.mem, &status) == H2_PAL_OK);
  assert(status.boot_intent == H2_LOADER_BOOT_INTENT_LOADER);
  assert(status.last_result == H2_PAL_OK);
}

/* Legacy tar.zlib with checksum and app/bk/app_ab_crc.rbl ("firmware"). */
static const uint8_t bk_test_archive[] = {
    0x78, 0x9c, 0xed, 0xd3, 0x41, 0x0a, 0xc2, 0x30, 0x10, 0x85, 0xe1, 0xae,
    0x3d, 0x85, 0x17, 0xd0, 0x26, 0x35, 0xc4, 0xe3, 0x94, 0x64, 0xa8, 0xb4,
    0xd4, 0x42, 0x49, 0xad, 0x5e, 0xbf, 0xa1, 0x28, 0xa2, 0x1b, 0x41, 0x6c,
    0xea, 0xe2, 0xff, 0x36, 0x13, 0x66, 0x33, 0x6f, 0x91, 0x27, 0x75, 0x25,
    0xed, 0x30, 0x76, 0xd9, 0x82, 0x54, 0x64, 0x8d, 0x99, 0x67, 0xf4, 0x3e,
    0x95, 0x2a, 0xf4, 0xf3, 0x3d, 0xef, 0x8f, 0xc6, 0x1e, 0xb2, 0xad, 0x5a,
    0x32, 0xd4, 0xc3, 0x38, 0x5c, 0x5c, 0x88, 0x27, 0x53, 0xdc, 0xfa, 0x43,
    0x2e, 0x48, 0xdd, 0x5c, 0xab, 0x9d, 0xdc, 0xff, 0xc1, 0x66, 0xed, 0x40,
    0x48, 0xca, 0xf5, 0x7d, 0xee, 0xdb, 0x3c, 0x8e, 0xd2, 0xf9, 0x52, 0x82,
    0xec, 0x83, 0x3f, 0xff, 0xf8, 0xc6, 0xc7, 0xfe, 0x6b, 0xf5, 0xda, 0x7f,
    0xad, 0xad, 0x2d, 0xe8, 0x7f, 0x0a, 0xa7, 0x26, 0x74, 0x37, 0x17, 0xaa,
    0xb5, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xe0, 0x3b, 0x13, 0xe9, 0x8f, 0x2d, 0x53,
};

static int archive_open(void *user, const char *path,
                        h2_pal_fs_open_mode_t mode,
                        h2_pal_fs_file_t **out_file) {
  (void)path;
  assert(mode == H2_PAL_FS_OPEN_READ);
  *(size_t *)user = 0u;
  *out_file = (h2_pal_fs_file_t *)user;
  return H2_PAL_OK;
}

static int archive_read(void *user, h2_pal_fs_file_t *file, void *data,
                        size_t len, size_t *out_read) {
  size_t *offset = user;
  (void)file;
  size_t take = sizeof(bk_test_archive) - *offset;
  if (take > len) take = len;
  memcpy(data, bk_test_archive + *offset, take);
  *offset += take;
  *out_read = take;
  return H2_PAL_OK;
}

static int archive_close(void *user, h2_pal_fs_file_t *file) {
  (void)user;
  (void)file;
  return H2_PAL_OK;
}

static void test_app_client_validates_target_archive_entry(void) {
  test_fixture_t fixture;
  h2_loader_app_client_t client;
  h2_loader_package_inspection_t inspection;
  size_t offset = 0u;
  static const h2_pal_fs_vtable_t fs_vtable = {
      .open = archive_open, .read = archive_read, .close = archive_close,
  };
  static const h2_pal_http_api_t http = {0};
  static const h2_pal_wifi_sta_api_t wifi = {0};
  static const h2_pal_disk_api_t disk = {0};
  const h2_pal_fs_api_t fs = {.user = &offset, .vtable = &fs_vtable};
  fixture_init(&fixture, 2u);
  h2_loader_app_client_config_t config = {
      .pref = &fixture.pref, .power = &fixture.power, .allocator = &fixture.mem,
      .fs = &fs, .http = &http, .wifi = &wifi, .disk = &disk,
      .digest = fixture.config.package.digest,
      .board = "devkit", .target = "esp32s3", .chip = "test",
      .active_identity = identity(H2_LOADER_IMAGE_ROLE_APP, SHA_A),
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1u, .app_partition_id = 2u,
      .now_ms = app_test_now, .sleep_ms = app_test_sleep,
  };
  /* The default ESP layout must continue rejecting another target's entry. */
  assert(h2_loader_app_client_init(&client, &config) == H2_PAL_OK);
  assert(h2_loader_package_inspect_path(&client.loader.package, "archive",
                                      &inspection) == H2_BUNDLE_ERR_LAYOUT);
  config.app_entry_path = "app/bk/app_ab_crc.rbl";
  assert(h2_loader_app_client_init(&client, &config) == H2_PAL_OK);
  assert(h2_loader_package_inspect_path(&client.loader.package, "archive",
                                      &inspection) == H2_PAL_OK);
  assert(strcmp(inspection.image_path, config.app_entry_path) == 0);
  assert(inspection.manifest.image_size == 8u);
}

int main(void) {
  test_app_client_package_entry_matches_loader();
  test_app_client_validates_target_archive_entry();
  test_empty_pref_preserves_default_boot_intent();
  test_max_status_fits_public_capacity();
  test_loader_intent_stays_and_seeds_partition_1();
  test_seed_running_metadata_preserves_package_origin();
  test_auto_without_partition_2_stays();
  test_auto_boots_valid_different_partition_2();
  test_partition_2_loader_copies_back_without_stage();
  test_partition_2_loader_preserves_stage_origin_on_both_copies();
  test_partition_2_copy_failure_keeps_partition_1_invalid();
  test_partition_2_copy_recovers_from_every_durable_boundary();
  test_converged_loader_finishes_stage();
  test_converged_loader_does_not_ignore_different_stage();
  test_same_image_new_package_is_still_inspected();
  test_rolled_back_app_leaves_partition_2_state_untouched();
  test_rolled_back_loader_candidate_is_not_relaunched();
  test_interrupted_replacement_does_not_boot_failed_app();
  test_app_finalize_only_consumes_matching_stage();
  test_app_confirmation_is_between_metadata_and_stage_cleanup();
  test_reboot_commands_only_set_intent_and_partition();
  test_reboot_preparation_and_failures_do_not_arm_boot();
  test_reboot_app_requires_bootable_partition_and_mfg_gate();
  test_stage_begin_invalidates_metadata_and_removes_old_package();
  test_mfg_variable_step_round_trip();
  test_mfg_legacy_records_decode_as_22_steps();
  test_mfg_acceptance_revision_tracks_total();
  test_mfg_status_prints_total_steps();
  test_mfg_required_total_range_and_gate();
  puts("h2loader v2 boot tests passed");
  return 0;
}
