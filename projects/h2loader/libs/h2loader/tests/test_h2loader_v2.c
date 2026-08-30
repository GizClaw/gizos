#include "h2_loader_boot.h"
#include "h2_loader_status.h"

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
  uint32_t boot_intent;
  int boot_intent_present;
  int32_t last_result;
  int last_result_present;
  int commit_result;
  unsigned commits;

  uint32_t running_partition;
  uint32_t next_partition;
  unsigned set_next_calls;
  unsigned reboot_calls;
  int set_next_result;
  int reboot_result;

  uint8_t partition_bytes[2][128];
  uint32_t writer_partition;
  size_t writer_offset;
  int writer_active;
  int writer_begin_result;
  int writer_write_result;
  int writer_finish_result;
  unsigned writer_aborts;
  uint8_t digest_byte;

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
  assert(*out_data != NULL);
  memcpy(*out_data, record->data, record->len);
  *out_len = record->len;
  return H2_PAL_OK;
}

static int pref_set_blob(h2_pal_pref_namespace_t *ns, const char *key,
                         const void *data, size_t len) {
  test_fixture_t *fixture = ns->user;
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
  if (strcmp(key, "boot_intent") != 0 || !fixture->boot_intent_present) {
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
  if (strcmp(key, "boot_intent") == 0) {
    fixture->boot_intent = value;
    fixture->boot_intent_present = 2;
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
  fixture->last_result = value;
  fixture->last_result_present = 2;
  return H2_PAL_OK;
}

static int pref_commit(h2_pal_pref_namespace_t *ns) {
  test_fixture_t *fixture = ns->user;
  ++fixture->commits;
  if (fixture->commit_result != H2_PAL_OK)
    return fixture->commit_result;
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
  if (fixture->boot_intent_present == 2)
    fixture->boot_intent_present = 1;
  if (fixture->last_result_present == 2)
    fixture->last_result_present = 1;
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

static int power_running(void *user,
                         h2_pal_power_boot_partition_t *out_partition) {
  test_fixture_t *fixture = user;
  memset(out_partition, 0, sizeof(*out_partition));
  out_partition->id = fixture->running_partition;
  return H2_PAL_OK;
}

static int power_next(void *user,
                      h2_pal_power_boot_partition_t *out_partition) {
  test_fixture_t *fixture = user;
  memset(out_partition, 0, sizeof(*out_partition));
  out_partition->id = fixture->next_partition;
  return H2_PAL_OK;
}

static int power_set_next(void *user, uint32_t partition_id) {
  test_fixture_t *fixture = user;
  ++fixture->set_next_calls;
  if (fixture->set_next_result != H2_PAL_OK)
    return fixture->set_next_result;
  fixture->next_partition = partition_id;
  return H2_PAL_OK;
}

static int power_reboot(void *user, uint32_t reason) {
  test_fixture_t *fixture = user;
  (void)reason;
  ++fixture->reboot_calls;
  return fixture->reboot_result;
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

static void fixture_init(test_fixture_t *fixture, uint32_t running_partition) {
  static const h2_pal_pref_vtable_t pref_vtable = {.open = pref_open};
  static const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .free = test_free,
  };
  static const h2_pal_fs_vtable_t fs_vtable = {.remove = fs_remove};
  static const h2_pal_power_vtable_t power_vtable = {
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
  fixture->set_next_result = H2_PAL_OK;
  fixture->reboot_result = H2_PAL_OK;
  fixture->writer_begin_result = H2_PAL_OK;
  fixture->writer_write_result = H2_PAL_OK;
  fixture->writer_finish_result = H2_PAL_OK;
  fixture->running_partition = running_partition;
  fixture->next_partition = running_partition;
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

static void test_reboot_commands_only_set_intent_and_partition(void) {
  test_fixture_t fixture;
  fixture_init(&fixture, 1u);
  assert(h2_loader_init(&fixture.loader, &fixture.config) == H2_PAL_OK);
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

int main(void) {
  test_max_status_fits_public_capacity();
  test_loader_intent_stays_and_seeds_partition_1();
  test_seed_running_metadata_preserves_package_origin();
  test_auto_without_partition_2_stays();
  test_auto_boots_valid_different_partition_2();
  test_partition_2_loader_copies_back_without_stage();
  test_partition_2_loader_preserves_stage_origin_on_both_copies();
  test_partition_2_copy_failure_keeps_partition_1_invalid();
  test_converged_loader_finishes_stage();
  test_converged_loader_does_not_ignore_different_stage();
  test_same_image_new_package_is_still_inspected();
  test_app_finalize_only_consumes_matching_stage();
  test_reboot_commands_only_set_intent_and_partition();
  test_stage_begin_invalidates_metadata_and_removes_old_package();
  puts("h2loader v2 boot tests passed");
  return 0;
}
