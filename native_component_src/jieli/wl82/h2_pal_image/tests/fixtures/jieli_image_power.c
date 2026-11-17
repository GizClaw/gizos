#include <assert.h>
#include <stdio.h>
#undef snprintf
#include <string.h>
#include "h2_jieli_wl82_platform_image.h"
#include "h2_jieli_wl82_boot.h"
#include "jieli_native_image.h"
#include "jieli_warm_boot.h"
#include "jieli_upgrade_io.h"
#include "h2_atomic.h"

typedef struct {
  int posts;
} OS_SEM;
#define OS_NO_ERR 0
#define CLEAR_APP_UPDATE_BANK 17
struct BootInfo {
  uint32_t baseAddress, codeLength, version, facts[7];
};
static struct BootInfo native = {0x4020u, 1024u, 7u, {1, 2, 3, 4, 5, 6, 7}};
static int native_error, warm_proof, erase_failed, exit_error, pend_error;
static uint32_t mapped_base = 0x4020u;
static h2_jieli_warm_request_t origin_request;
uint32_t h2_jieli_warm_boot_take_rom_base(void) {
  return h2_jieli_warm_request_take_origin(&origin_request, mapped_base,
                                           0x4020u, 0x37c020u);
}
static unsigned creates, inits, exits, clears, burns, arms, publications;
static unsigned minimum_publications_before_clear;
static unsigned native_writes, native_bytes, first_write_size, reset_calls,
    warm_requests;
static int (*late_completion)(int);
static void (*reset_callback)(void*);
static void* reset_argument;
static unsigned timer_allocation = 7u;
static uint8_t deferred_header[32];

const h2_jieli_wl82_boot_geometry_t* h2_jieli_wl82_boot_geometry(void) {
  static const h2_jieli_wl82_boot_geometry_t geometry = {0x4020u, 0x37c020u,
                                                         0x740000u};
  return &geometry;
}
uint32_t h2_jieli_warm_boot_running_base(int rc, uint32_t base) {
  if (mapped_base != 0x4020u && mapped_base != 0x37c020u) return 0;
  if (rc == 0) return base == mapped_base ? mapped_base : 0;
  return warm_proof && mapped_base == 0x37c020u ? mapped_base : 0;
}
void h2_jieli_warm_boot_report(void (*write)(const char*)) { (void)write; }
void h2_jieli_wl82_warm_boot_request(uint32_t base) {
  assert(base == 0x37c020u);
  ++warm_requests;
}
static int get_current_boot_info(struct BootInfo* out) {
  *out = native;
  return native_error;
}
static uint32_t dual_bank_passive_update_init(uint32_t crc, uint32_t size,
                                              uint32_t block, void* callback) {
  assert(crc == 0 && size == 0 && block == 4096 && callback == NULL);
  ++inits;
  return 0;
}
static uint32_t dual_bank_update_allow_check(uint32_t value) {
  assert(value == 0);
  return 0;
}
uint32_t get_target_udate_addr(void) { return 0x123u; }
static uint32_t dual_bank_passive_update_exit(void* callback) {
  assert(callback == NULL);
  ++exits;
  return exit_error;
}
static int flash_update_clr_boot_info(int bank) {
  assert(bank == 17 && publications >= minimum_publications_before_clear);
  ++clears;
  return 0;
}
static uint32_t dual_bank_update_write(void* data, uint16_t size,
                                       int (*error)(void*)) {
  assert(data != NULL && size != 0);
  if (native_writes++ == 0) {
    first_write_size = size;
    assert(error == NULL);
  }
  native_bytes += size;
  return 0;
}
static uint32_t dual_bank_update_burn_boot_info(int (*callback)(int)) {
  ++burns;
  late_completion = callback;
  if (!pend_error) callback(0);
  return 0;
}
uint32_t decode_data_by_user_key(uint16_t key, uint8_t* data, uint16_t size,
                                 uint32_t addr, uint8_t block) {
  (void)data;
  assert(key == 0xffffu && size == 32 && addr == 0 && block == 32);
  return 0;
}
int h2_jieli_upgrade_erase_failed(void) { return erase_failed; }
int h2_jieli_upgrade_header_arm(void) {
  ++arms;
  return 0;
}
int h2_jieli_upgrade_header_copy(uint8_t out[32]) {
  memcpy(out, deferred_header, 32);
  return 0;
}
int h2_jieli_upgrade_header_publish(const uint8_t header[32]) {
  assert(mapped_base == 0x37c020u && memcmp(header, deferred_header, 32) == 0);
  ++publications;
  return 0;
}
static int os_sem_create(OS_SEM* sem, int value) {
  sem->posts = value;
  ++creates;
  return 0;
}
static int os_sem_set(OS_SEM* sem, int value) {
  sem->posts = value;
  return 0;
}
static int os_sem_post(OS_SEM* sem) {
  ++sem->posts;
  return 0;
}
static int os_sem_pend(OS_SEM* sem, unsigned ticks) {
  assert(ticks == 500u);
  if (pend_error || sem->posts == 0) return -1;
  --sem->posts;
  return 0;
}
static void os_time_dly(unsigned ticks) { (void)ticks; }
static void system_reset(void) { ++reset_calls; }
static unsigned sys_timeout_add_to_task(const char* name, void* user,
                                        void (*callback)(void*),
                                        unsigned delay) {
  assert(strcmp(name, "sys_timer") == 0 && delay == 2000);
  reset_callback = callback;
  reset_argument = user;
  return timer_allocation;
}

struct h2_pal_fs_file {
  int index;
  size_t position;
  int open;
};
static struct {
  uint8_t bytes[8192];
  size_t size;
  int exists;
} files[3];
static struct h2_pal_fs_file handles[3];
static unsigned opens, closes;
static int read_error;
static int file_index(const char* path) {
  if (strcmp(path, "temporary") == 0) return 0;
  if (strcmp(path, "one") == 0) return 1;
  if (strcmp(path, "two") == 0) return 2;
  return -1;
}
static int file_open(void* user, const char* path, h2_pal_fs_open_mode_t mode,
                     h2_pal_fs_file_t** out) {
  (void)user;
  int index = file_index(path);
  assert(index >= 0 && !handles[index].open);
  if (mode == H2_PAL_FS_OPEN_READ && !files[index].exists)
    return H2_PAL_ERR_NOT_FOUND;
  if (mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE) {
    files[index].exists = 1;
    files[index].size = 0;
  }
  handles[index] = (struct h2_pal_fs_file){index, 0, 1};
  *out = &handles[index];
  ++opens;
  return 0;
}
static int file_close(void* user, h2_pal_fs_file_t* file) {
  (void)user;
  assert(file->open);
  file->open = 0;
  ++closes;
  return 0;
}
static int file_read(void* user, h2_pal_fs_file_t* file, void* out, size_t size,
                     size_t* count) {
  (void)user;
  assert(file->open);
  if (read_error) return H2_PAL_ERR_IO;
  assert(size <= files[file->index].size - file->position);
  memcpy(out, files[file->index].bytes + file->position, size);
  file->position += size;
  *count = size;
  return 0;
}
static int file_seek(void* user, h2_pal_fs_file_t* file, uint64_t offset) {
  (void)user;
  assert(file->open && offset <= files[file->index].size);
  file->position = (size_t)offset;
  return 0;
}
static int file_write(void* user, h2_pal_fs_file_t* file, const void* data,
                      size_t size, size_t* count) {
  (void)user;
  assert(file->open && size <= 8192 - files[file->index].size);
  memcpy(files[file->index].bytes + file->position, data, size);
  file->position += size;
  files[file->index].size = file->position;
  *count = size;
  return 0;
}
static int file_sync(void* user, h2_pal_fs_file_t* file) {
  (void)user;
  assert(file->open);
  return 0;
}
static int file_stat(void* user, const char* path, h2_pal_fs_stat_t* out) {
  (void)user;
  int index = file_index(path);
  assert(index >= 0);
  if (!files[index].exists) return H2_PAL_ERR_NOT_FOUND;
  *out = (h2_pal_fs_stat_t){files[index].size, 0};
  return 0;
}
static int file_remove(void* user, const char* path) {
  (void)user;
  int index = file_index(path);
  assert(index >= 0 && !handles[index].open);
  if (!files[index].exists) return H2_PAL_ERR_NOT_FOUND;
  files[index].exists = 0;
  return 0;
}
static int file_rename(void* user, const char* from, const char* to) {
  (void)user;
  int source = file_index(from), dest = file_index(to);
  assert(source >= 0 && dest >= 0);
  assert(!handles[source].open && !handles[dest].open);
  files[dest] = files[source];
  files[source].exists = 0;
  return 0;
}
static const h2_pal_fs_vtable_t fs_vtable = {.open = file_open,
                                             .close = file_close,
                                             .read = file_read,
                                             .seek = file_seek,
                                             .write = file_write,
                                             .sync = file_sync,
                                             .stat = file_stat,
                                             .remove = file_remove,
                                             .rename = file_rename};
static const h2_pal_fs_api_t fs = {.vtable = &fs_vtable};

/* PROVIDER */

static uint8_t ufw[8192];
static void put32(uint8_t* p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
static void fix_crc(uint8_t* p) {
  uint16_t crc = h2_jieli_native_header_crc(p);
  p[0] = (uint8_t)crc;
  p[1] = (uint8_t)(crc >> 8);
}
static void build_ufw(void) {
  memset(ufw, 0xa5, sizeof(ufw));
  memset(ufw, 0, 64);
  put32(ufw + 4, 32);
  put32(ufw + 8, sizeof(ufw));
  ufw[14] = 1;
  memcpy(ufw + 16, "update_data", 12);
  fix_crc(ufw);
  put32(ufw + 36, 64);
  put32(ufw + 40, sizeof(ufw) - 64);
  ufw[34] = 0x0d;
  ufw[35] = 0x68;
  memcpy(ufw + 48, "app_core", 9);
  fix_crc(ufw + 32);
  memset(deferred_header, 0, 32);
  deferred_header[4] = 0x0d;
  deferred_header[5] = 0x68;
  put32(deferred_header + 8, sizeof(ufw) - 64);
  fix_crc(deferred_header);
}
static void write_candidate(void) {
  const h2_pal_image_write_request_t request = {2, 1, sizeof(ufw)};
  assert(h2_pal_image_begin(h2_jieli_wl82_platform_image_api(), &request) == 0);
  assert(h2_pal_image_begin(h2_jieli_wl82_platform_image_api(), &request) ==
         H2_PAL_ERR_BUSY);
  assert(inits == 1);
  assert(h2_pal_power_set_next_boot_partition(
             h2_jieli_wl82_platform_power_api(), 2) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_image_write(h2_jieli_wl82_platform_image_api(), ufw, 4096) ==
         0);
  assert(native_writes == 1 && first_write_size == 4096 - 0x123u);
  assert(h2_pal_image_write(h2_jieli_wl82_platform_image_api(), ufw + 4096,
                            4096) == 0);
  assert(h2_pal_image_finish(h2_jieli_wl82_platform_image_api()) == 0);
  assert(native_bytes == sizeof(ufw) && state.update_active &&
         state.writer_partition_id == 0);
  assert(burns == 0 && files[2].exists && state.committed_size == sizeof(ufw));
}
int main(int argc, char** argv) {
  assert(argc == 2);
  build_ufw();
  const h2_jieli_wl82_image_config_t config = {
      &fs, 1, 2, 0x37d000u, "temporary", "one", "two"};
  int existing_bank_one = strcmp(argv[1], "existing_bank_one") == 0 ||
                          strcmp(argv[1], "confirmed_bank_one") == 0 ||
                          strcmp(argv[1], "warm_bank_one") == 0;
  int warm_return = strncmp(argv[1], "return_warm_", 12) == 0;
  if (warm_return) {
    mapped_base = 0x37c020u;
    native_error = warm_proof = 1;
    origin_request.sfc_base = origin_request.last_base = mapped_base;
    origin_request.check = h2_jieli_warm_request_check(mapped_base);
    origin_request.inflight = H2_JIELI_WARM_INFLIGHT;
    origin_request.count = 3u;
    h2_jieli_warm_request_record_origin(
        &origin_request, strcmp(argv[1], "return_warm_wrong_origin") == 0
                             ? mapped_base
                             : 0x4020u);
    if (strcmp(argv[1], "return_warm_torn") == 0) ++origin_request.origin_check;
  }
  if (existing_bank_one) {
    mapped_base = native.baseAddress = 0x37c020u;
    if (strcmp(argv[1], "warm_bank_one") == 0) {
      native_error = 1;
      warm_proof = 1;
    }
  }
  if (strcmp(argv[1], "failed_mapping") == 0) {
    native_error = 1;
    assert(h2_jieli_wl82_platform_image_configure(&config) == H2_PAL_ERR_IO);
    assert(!state.configured && state.running_partition_id == 0 &&
           creates == 1);
    mapped_base = 0x37c020u;
    warm_proof = 1;
    assert(h2_jieli_wl82_platform_image_configure(&config) == 0 &&
           state.running_partition_id == 2 && creates == 1);
    return 0;
  }
  assert(h2_jieli_wl82_platform_image_configure(&config) == 0);
  assert(h2_jieli_wl82_platform_image_configure(&config) == 0 && creates == 1);
  const h2_pal_image_api_t* image = h2_jieli_wl82_platform_image_api();
  const h2_pal_power_api_t* power = h2_jieli_wl82_platform_power_api();
  h2_pal_power_capabilities_t capabilities;
  assert(h2_pal_power_get_capabilities(power, &capabilities) == H2_PAL_OK);
  const uint32_t image_capabilities = H2_PAL_POWER_CAPABILITY_IMAGE_STATE |
                                      H2_PAL_POWER_CAPABILITY_PREPARE_BOOT |
                                      H2_PAL_POWER_CAPABILITY_CONFIRM_IMAGE;
  assert((capabilities.flags & image_capabilities) == image_capabilities);
  /* App preflight relies on role flags from the running query, not only from
   * partition enumeration. Next/running must describe the same partition. */
  h2_pal_power_boot_partition_t running_partition, next_partition;
  assert(h2_pal_power_get_running_boot_partition(power, &running_partition) ==
         H2_PAL_OK);
  const uint32_t role_flag = running_partition.id == 2u
                                 ? H2_PAL_POWER_BOOT_PARTITION_FLAG_APP
                                 : H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY;
  assert((running_partition.flags & role_flag) != 0u);
  assert((running_partition.flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING) !=
         0u);
  assert(h2_pal_power_get_next_boot_partition(power, &next_partition) ==
         H2_PAL_OK);
  assert(next_partition.id == running_partition.id &&
         (next_partition.flags & role_flag) != 0u);
  if (warm_return) {
    assert(state.running_partition_id == 2 &&
           origin_request.origin_magic == 0u);
    assert(h2_jieli_warm_boot_take_rom_base() ==
           0u); /* receipt cannot replay */
    if (strcmp(argv[1], "return_warm_confirmed") == 0) {
      h2_pal_power_image_confirmation_t confirmation;
      assert(h2_pal_power_confirm_running_image(power, NULL, &confirmation) ==
             0);
    }
    if (strcmp(argv[1], "return_warm_updated") == 0) {
      const h2_pal_image_write_request_t update = {1, 2, sizeof(ufw)};
      assert(h2_pal_image_begin(image, &update) == 0);
      assert(h2_pal_image_abort(image) == 0);
    }
    const h2_pal_power_boot_request_t request = {
        1, H2_PAL_POWER_BOOT_POLICY_EXISTING};
    h2_pal_power_native_evidence_t evidence;
    assert(h2_pal_power_prepare_next_boot_partition(power, &request,
                                                    &evidence) == 0);
    int reject = strcmp(argv[1], "return_warm_wrong_origin") == 0 ||
                 strcmp(argv[1], "return_warm_torn") == 0 ||
                 strcmp(argv[1], "return_warm_updated") == 0;
    assert(h2_pal_power_set_next_boot_partition(power, 1) ==
           (reject ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_OK));
    h2_pal_power_boot_partition_t next;
    assert(h2_pal_power_get_next_boot_partition(power, &next) == 0 &&
           next.id == (reject ? 2u : 1u));
    assert(h2_pal_power_reboot(power, 0) == 0);
    reset_callback(reset_argument);
    assert(reset_calls == 1 && warm_requests == (reject ? 1u : 0u));
    return 0;
  }
  if (existing_bank_one) {
    h2_pal_power_boot_partition_t partition;
    assert(h2_pal_power_get_running_boot_partition(power, &partition) == 0 &&
           partition.id == 2);
    if (strcmp(argv[1], "confirmed_bank_one") == 0) {
      h2_pal_power_image_confirmation_t confirmed;
      assert(h2_pal_power_confirm_running_image(power, NULL, &confirmed) == 0);
      assert(confirmed.partition_id == 2);
    }
    uint8_t before[sizeof(state)];
    memcpy(before, &state, sizeof(state));
    const h2_pal_power_boot_request_t request = {
        1, H2_PAL_POWER_BOOT_POLICY_EXISTING};
    h2_pal_power_native_evidence_t evidence;
    assert(h2_pal_power_prepare_next_boot_partition(power, &request,
                                                    &evidence) == 0);
    assert(evidence.format == 0 && evidence.size == 0);
    assert(memcmp(before, &state, sizeof(state)) == 0);
    assert(h2_pal_power_set_next_boot_partition(power, 1) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(memcmp(before, &state, sizeof(state)) == 0);
    assert(h2_pal_power_get_next_boot_partition(power, &partition) == 0 &&
           partition.id == 2 &&
           (partition.flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT));
    assert(burns == 0 && clears == 0 && publications == 0 && exits == 0 &&
           opens == 0 && closes == 0 && native_writes == 0 &&
           warm_requests == 0 && reset_calls == 0);
    assert(h2_pal_power_reboot(power, 0) == 0 && warm_requests == 0);
    reset_callback(reset_argument);
    assert(warm_requests == 1 && reset_calls == 1);
    return 0;
  }
  if (strcmp(argv[1], "cancel_warm_trial") == 0) {
    h2_pal_power_boot_partition_t partition;
    assert(h2_pal_power_set_next_boot_partition(power, 2) == 0);
    assert(h2_pal_power_get_next_boot_partition(power, &partition) == 0 &&
           partition.id == 2 && warm_requests == 0);
    assert(h2_pal_power_set_next_boot_partition(power, 1) == 0);
    assert(h2_pal_power_get_next_boot_partition(power, &partition) == 0 &&
           partition.id == 1);
    assert(h2_pal_power_reboot(power, 0) == 0 && warm_requests == 0);
    reset_callback(reset_argument);
    assert(reset_calls == 1 && warm_requests == 0 && burns == 0 &&
           clears == 0 && publications == 0 && native_writes == 0);
    return 0;
  }
  h2_pal_image_info_t info;
  assert(h2_pal_image_get_info(image, 1, H2_PAL_IMAGE_VIEW_STORED, &info) == 0);
  assert(info.capacity == 0x37d000u &&
         !(info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN));
  assert(h2_pal_image_get_info(image, 1, H2_PAL_IMAGE_VIEW_NATIVE, &info) ==
             0 &&
         info.image_size == 1024u);
  if (strcmp(argv[1], "short_tail") == 0 || strcmp(argv[1], "small") == 0) {
    int crosses = strcmp(argv[1], "short_tail") == 0;
    uint32_t size = crosses ? 3999u : 200u;
    put32(ufw + 8, size);
    fix_crc(ufw);
    put32(ufw + 40, size - 64u);
    fix_crc(ufw + 32);
    const h2_pal_image_write_request_t request = {2, 1, size};
    assert(h2_pal_image_begin(image, &request) == 0);
    assert(h2_pal_image_write(image, ufw, size) == 0 && native_writes == 0);
    int result = h2_pal_image_finish(image);
    if (crosses) {
      assert(result == H2_PAL_ERR_TRUNCATED && native_bytes == 4096u - 0x123u);
      assert(!files[0].exists && !files[2].exists && !state.update_active);
      assert(state.committed_partition_id == 0 && clears == 1 && burns == 0);
      const h2_pal_power_boot_request_t boot = {2,
                                                H2_PAL_POWER_BOOT_POLICY_TRIAL};
      h2_pal_power_native_evidence_t evidence;
      assert(h2_pal_power_prepare_next_boot_partition(
                 power, &boot, &evidence) == H2_PAL_ERR_INVALID_STATE);
    } else {
      assert(result == 0 && native_bytes == size &&
             state.update_buffered == 0 && files[2].exists);
      assert(state.update_active && state.committed_size == size && burns == 0);
      assert(h2_pal_image_abort(image) == 0);
    }
    return 0;
  }
  write_candidate();
  if (strcmp(argv[1], "stored") == 0) {
    assert(h2_pal_image_get_info(image, 2, H2_PAL_IMAGE_VIEW_STORED, &info) ==
           0);
    assert(info.image_size == sizeof(ufw) &&
           (info.flags & H2_PAL_IMAGE_FLAG_PREPARED));
    uint8_t data[8192];
    assert(h2_pal_image_read(image, 2, H2_PAL_IMAGE_VIEW_STORED, 0, data,
                             100) == 0);
    assert(state.reader != NULL && state.update_active);
    assert(h2_pal_image_read(image, 2, H2_PAL_IMAGE_VIEW_STORED, 100,
                             data + 100, sizeof(data) - 100) == 0);
    assert(state.reader == NULL && memcmp(data, ufw, sizeof(data)) == 0 &&
           state.update_active);
    read_error = 1;
    assert(h2_pal_image_read(image, 2, H2_PAL_IMAGE_VIEW_STORED, 0, data, 1) ==
           H2_PAL_ERR_IO);
    assert(state.reader == NULL);
    read_error = 0;
    assert(h2_pal_image_abort(image) == 0 && clears == 1 && !files[2].exists &&
           !state.update_active);
    assert(h2_pal_image_abort(image) == 0 && clears == 2);
  } else if (strcmp(argv[1], "readonly") == 0) {
    uint8_t before[sizeof(state)];
    memcpy(before, &state, sizeof(state));
    const h2_pal_power_boot_request_t request = {
        1, H2_PAL_POWER_BOOT_POLICY_EXISTING};
    h2_pal_power_native_evidence_t evidence;
    assert(h2_pal_power_prepare_next_boot_partition(power, &request,
                                                    &evidence) == 0);
    assert(evidence.format == 2 && evidence.size == sizeof(native) &&
           memcmp(evidence.bytes, &native, sizeof(native)) == 0);
    assert(memcmp(before, &state, sizeof(state)) == 0 && burns == 0 &&
           exits == 0 && clears == 0);
  } else if (strcmp(argv[1], "cleanup") == 0) {
    exit_error = 1;
    assert(h2_pal_image_abort(image) == H2_PAL_ERR_IO && clears == 1 &&
           state.poisoned);
    const h2_pal_image_write_request_t request = {2, 1, sizeof(ufw)};
    assert(h2_pal_image_begin(image, &request) == H2_PAL_ERR_IO);
  } else {
    int trial = strcmp(argv[1], "trial") == 0;
    int late = strcmp(argv[1], "late") == 0;
    assert(trial || late || strcmp(argv[1], "recovery") == 0);
    const h2_pal_power_boot_request_t request = {
        2, trial ? H2_PAL_POWER_BOOT_POLICY_TRIAL
                 : H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY};
    h2_pal_power_native_evidence_t evidence;
    if (late) {
      pend_error = 1;
      assert(h2_pal_power_prepare_next_boot_partition(
                 power, &request, &evidence) == H2_PAL_ERR_TIMEOUT);
      int result = state.update_result;
      late_completion(1);
      assert(state.update_result == result && state.update_sem.posts == 1);
      const h2_pal_image_write_request_t again = {2, 2, sizeof(ufw)};
      pend_error = 0;
      assert(h2_pal_image_begin(image, &again) == 0 &&
             state.update_sem.posts == 0);
      assert(creates == 1);
      return 0;
    }
    assert(h2_pal_power_prepare_next_boot_partition(power, &request,
                                                    &evidence) == 0);
    assert(!state.update_active && exits == 1 && publications == 0);
    if (trial)
      assert(evidence.size == 0 && burns == 0 && arms == 0);
    else
      assert(evidence.format == 1 && evidence.size == 40 && burns == 1 &&
             arms == 1);
    h2_pal_power_native_evidence_t duplicate;
    assert(h2_pal_power_prepare_next_boot_partition(
               power, &request, &duplicate) == H2_PAL_ERR_INVALID_STATE &&
           duplicate.size == 0);
    assert(h2_pal_power_set_next_boot_partition(power, 2) == 0);
    assert(publications == 0 && burns == (trial ? 0u : 1u));
    timer_allocation = 0;
    assert(h2_pal_power_reboot(power, 0) == H2_PAL_ERR_NO_MEMORY &&
           warm_requests == 0);
    timer_allocation = 7;
    assert(h2_pal_power_reboot(power, 0) == 0 && warm_requests == 0 &&
           reset_calls == 0);
    assert(h2_pal_power_reboot(power, 0) == 0);
    reset_callback(reset_argument);
    assert(warm_requests == 1 && reset_calls == 1);
    state.reboot_timer_id = 0;
    if (!trial) {
      state.running_partition_id = 2;
      mapped_base = 0x37c020u;
      native_error = 1;
      warm_proof = 1;
      assert(h2_pal_power_set_next_boot_partition(power, 2) ==
             H2_PAL_ERR_INVALID_STATE);
      h2_pal_power_image_confirmation_t confirmed;
      assert(h2_pal_power_confirm_running_image(power, &evidence, &confirmed) ==
                 0 &&
             confirmed.was_pending);
      assert(h2_pal_power_set_next_boot_partition(power, 2) == 0 &&
             publications == 1);
      unsigned old_clears = clears;
      const h2_pal_image_write_request_t copy = {1, 1, sizeof(ufw)};
      assert(h2_pal_image_begin(image, &copy) == 0 && clears == old_clears &&
             files[2].exists);
      assert(state.update_active && inits == 2);
      assert(h2_pal_image_write(image, ufw, 4096) == 0);
      assert(h2_pal_image_write(image, ufw + 4096, 4096) == 0);
      assert(h2_pal_image_finish(image) == 0);
      const h2_pal_power_boot_request_t canonical = {
          1, H2_PAL_POWER_BOOT_POLICY_EXISTING};
      h2_pal_power_native_evidence_t unused;
      assert(h2_pal_power_prepare_next_boot_partition(power, &canonical,
                                                      &unused) == 0);
      assert(h2_pal_power_set_next_boot_partition(power, 1) == 0 &&
             state.update_committed && state.warm_partition_id == 0);
      assert(files[1].exists && files[2].exists &&
             state.next_partition_id == 1);
      timer_allocation = 0;
      assert(h2_pal_power_reboot(power, 0) == H2_PAL_ERR_NO_MEMORY);
      minimum_publications_before_clear = publications + 1u;
      assert(h2_pal_power_set_next_boot_partition(power, 2) == 0);
      assert(clears == old_clears + 1u && state.next_partition_id == 2 &&
             !state.update_committed);
      assert(state.committed_partition_id == 0 && files[1].exists &&
             files[2].exists);
    }
  }
  assert(opens == closes || state.shadow != NULL);
  return 0;
}
