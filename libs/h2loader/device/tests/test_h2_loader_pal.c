#include "h2_loader_pal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  uint8_t bytes[64];
  uint64_t size;
  int known_size;
  uint32_t running;
  h2_pal_image_write_request_t request;
  size_t written;
  unsigned begins;
  unsigned finishes;
  unsigned aborts;
  unsigned sleeps;
  unsigned selections;
  unsigned confirmations;
  unsigned allocations;
  int prepared;
  h2_pal_result_t abort_result;
  h2_pal_result_t sleep_result;
} fixture_t;

static h2_pal_result_t get_info(void* user, uint32_t partition_id,
                                h2_pal_image_view_t view,
                                h2_pal_image_info_t* out_info) {
  fixture_t* fixture = user;
  assert(view == H2_PAL_IMAGE_VIEW_STORED);
  if (partition_id != 1u && partition_id != 2u) return H2_PAL_ERR_NOT_FOUND;
  out_info->capacity = sizeof(fixture->bytes);
  out_info->image_size = fixture->known_size ? fixture->size : 0u;
  out_info->flags = H2_PAL_IMAGE_FLAG_READABLE | H2_PAL_IMAGE_FLAG_WRITABLE;
  if (fixture->known_size) out_info->flags |= H2_PAL_IMAGE_FLAG_SIZE_KNOWN;
  return H2_PAL_OK;
}

static h2_pal_result_t read_image(void* user, uint32_t partition_id,
                                  h2_pal_image_view_t view, uint64_t offset,
                                  void* data, size_t len) {
  fixture_t* fixture = user;
  assert(partition_id == 1u || partition_id == 2u);
  assert(view == H2_PAL_IMAGE_VIEW_STORED);
  if (offset > sizeof(fixture->bytes) ||
      len > sizeof(fixture->bytes) - (size_t)offset)
    return H2_PAL_ERR_INVALID_ARG;
  memcpy(data, fixture->bytes + (size_t)offset, len);
  return H2_PAL_OK;
}

static h2_pal_result_t begin_image(
    void* user, const h2_pal_image_write_request_t* request) {
  fixture_t* fixture = user;
  if (request->partition_id == fixture->running)
    return H2_PAL_ERR_INVALID_STATE;
  fixture->request = *request;
  fixture->written = 0u;
  ++fixture->begins;
  return H2_PAL_OK;
}

static h2_pal_result_t write_image(void* user, const void* data, size_t len) {
  fixture_t* fixture = user;
  if (len > sizeof(fixture->bytes) - fixture->written)
    return H2_PAL_ERR_NO_SPACE;
  memcpy(fixture->bytes + fixture->written, data, len);
  fixture->written += len;
  return H2_PAL_OK;
}

static h2_pal_result_t finish_image(void* user) {
  fixture_t* fixture = user;
  ++fixture->finishes;
  fixture->prepared = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t abort_image(void* user) {
  fixture_t* fixture = user;
  ++fixture->aborts;
  if (fixture->abort_result == H2_PAL_OK) fixture->prepared = 0;
  return fixture->abort_result;
}

static h2_pal_result_t running_partition(
    void* user, h2_pal_power_boot_partition_t* out_partition) {
  fixture_t* fixture = user;
  memset(out_partition, 0, sizeof(*out_partition));
  out_partition->id = fixture->running;
  return H2_PAL_OK;
}

static h2_pal_result_t select_partition(void* user, uint32_t partition_id) {
  fixture_t* fixture = user;
  (void)partition_id;
  ++fixture->selections;
  return H2_PAL_OK;
}

static h2_pal_result_t confirm_image(
    void* user, const h2_pal_power_native_evidence_t* evidence,
    h2_pal_power_image_confirmation_t* out_confirmation) {
  fixture_t* fixture = user;
  assert(evidence == NULL);
  ++fixture->confirmations;
  out_confirmation->partition_id = fixture->running;
  out_confirmation->was_pending = 1u;
  return H2_PAL_OK;
}

static h2_pal_result_t sleep_ms(void* user, uint32_t ms) {
  fixture_t* fixture = user;
  assert(ms == 1u);
  ++fixture->sleeps;
  return fixture->sleep_result;
}

static void* allocate(void* user, size_t size) {
  fixture_t* fixture = user;
  ++fixture->allocations;
  return malloc(size);
}

static void release(void* user, void* pointer) {
  fixture_t* fixture = user;
  assert(fixture->allocations != 0u);
  --fixture->allocations;
  free(pointer);
}

static void check_adapters(void) {
  fixture_t fixture = {.bytes = {'a', 'b', 'c', 'x', 'x'},
                       .size = 3u,
                       .known_size = 1,
                       .running = 1u};
  static const h2_pal_image_vtable_t image_vtable = {
      .get_info = get_info,
      .read = read_image,
      .begin = begin_image,
      .write = write_image,
      .finish = finish_image,
      .abort = abort_image,
  };
  static const h2_pal_power_vtable_t power_vtable = {
      .get_running_boot_partition = running_partition,
      .set_next_boot_partition = select_partition,
      .confirm_running_image = confirm_image,
  };
  static const h2_pal_time_vtable_t time_vtable = {.sleep_ms = sleep_ms};
  static const h2_pal_mem_vtable_t mem_vtable = {.alloc = allocate,
                                                 .free = release};
  const h2_pal_image_api_t image = {.user = &fixture, .vtable = &image_vtable};
  const h2_pal_power_api_t power = {.user = &fixture, .vtable = &power_vtable};
  const h2_pal_time_api_t time = {.user = &fixture, .vtable = &time_vtable};
  const h2_pal_mem_api_t mem = {.user = &fixture, .vtable = &mem_vtable};
  h2_loader_pal_config_t config = {.image = &image,
                                   .power = &power,
                                   .time = &time,
                                   .allocator = &mem,
                                   .loader_partition_id = 1u,
                                   .app_partition_id = 2u,
                                   .digest_yield_ms = 1u};
  h2_loader_pal_t state;
  assert(h2_loader_pal_init(&state, &config) == H2_PAL_OK);
  uint64_t capacity = 0u;
  assert(state.image_reader.vtable->get_capacity(state.image_reader.user, 1u,
                                                 &capacity) == H2_PAL_OK);
  assert(capacity == sizeof(fixture.bytes));
  uint8_t bytes[3];
  assert(state.image_reader.vtable->read(state.image_reader.user, 1u, 0u, bytes,
                                         sizeof(bytes)) == H2_PAL_OK);
  assert(memcmp(bytes, "abc", sizeof(bytes)) == 0);

  h2_loader_image_identity_t identity;
  assert(h2_loader_pal_current_image_identity(&state, H2_LOADER_IMAGE_ROLE_APP,
                                              "board", "app", "v1",
                                              &identity) == H2_PAL_OK);
  assert(identity.image_size == 3u);
  assert(
      strcmp(
          identity.image_sha256,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") ==
      0);
  assert(fixture.sleeps == 1u && fixture.allocations == 0u);
  fixture.known_size = 0;
  assert(h2_loader_pal_current_image_identity(
             &state, H2_LOADER_IMAGE_ROLE_APP, "board", "app", "v1",
             &identity) == H2_PAL_ERR_UNSUPPORTED);
  assert(identity.image_size == 0u && identity.image_sha256[0] == '\0');
  assert(fixture.allocations == 0u && fixture.sleeps == 1u);
  fixture.known_size = 1;
  fixture.sleep_result = H2_PAL_ERR_IO;
  assert(h2_loader_pal_current_image_identity(&state, H2_LOADER_IMAGE_ROLE_APP,
                                              "board", "app", "v1",
                                              &identity) == H2_PAL_ERR_IO);
  assert(fixture.allocations == 0u && state.sha_state.active == 0);
  fixture.sleep_result = H2_PAL_OK;

  const h2_loader_image_writer_api_t* writer = &state.image_writer;
  identity = (h2_loader_image_identity_t){.role = H2_LOADER_IMAGE_ROLE_H2LOADER,
                                          .image_size = 3u};
  assert(writer->vtable->begin(writer->user, 2u, &identity) == H2_PAL_OK);
  assert(fixture.request.partition_id == 2u &&
         fixture.request.execution_partition_id == 1u &&
         fixture.request.image_size == 3u);
  assert(writer->vtable->write(writer->user, "ab", 2u) == H2_PAL_OK);
  assert(writer->vtable->finish(writer->user, &identity) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(writer->vtable->write(writer->user, "cd", 2u) == H2_PAL_ERR_NO_SPACE);
  assert(fixture.written == 2u && fixture.finishes == 0u);
  assert(writer->vtable->write(writer->user, "c", 1u) == H2_PAL_OK);
  assert(writer->vtable->finish(writer->user, &identity) == H2_PAL_OK);
  assert(fixture.prepared && fixture.selections == 0u && fixture.aborts == 0u);
  writer->vtable->abort(writer->user);
  assert(!fixture.prepared && fixture.aborts == 1u);

  identity.role = H2_LOADER_IMAGE_ROLE_APP;
  assert(writer->vtable->begin(writer->user, 2u, &identity) == H2_PAL_OK);
  assert(fixture.request.execution_partition_id == 2u);
  assert(writer->vtable->write(writer->user, "abc", 3u) == H2_PAL_OK);
  assert(writer->vtable->finish(writer->user, &identity) == H2_PAL_OK);
  /* A fresh canonical copy must not cancel the activated recovery image. */
  fixture.running = 2u;
  identity.role = H2_LOADER_IMAGE_ROLE_H2LOADER;
  assert(writer->vtable->begin(writer->user, 1u, &identity) == H2_PAL_OK);
  assert(fixture.aborts == 1u && fixture.prepared);
  fixture.abort_result = H2_PAL_ERR_IO;
  writer->vtable->abort(writer->user);
  assert(state.last_abort_result == H2_PAL_ERR_IO);
  const unsigned begins = fixture.begins;
  assert(writer->vtable->begin(writer->user, 1u, &identity) == H2_PAL_ERR_IO);
  assert(fixture.begins == begins);
  fixture.abort_result = H2_PAL_OK;
  writer->vtable->abort(writer->user);
  assert(state.last_abort_result == H2_PAL_OK && !fixture.prepared);
  assert(fixture.selections == 0u);
  assert(h2_loader_pal_confirm_running_image((void*)&power) == H2_PAL_OK);
  assert(fixture.confirmations == 1u);

  config.time = NULL;
  assert(h2_loader_pal_init(&state, &config) == H2_PAL_ERR_INVALID_ARG);
  config.digest_yield_ms = 0u;
  assert(h2_loader_pal_init(&state, &config) == H2_PAL_OK);
  /* In-place configuration reuse is safe. */
  assert(h2_loader_pal_init(&state, &state.config) == H2_PAL_OK);
  config.app_partition_id = 1u;
  assert(h2_loader_pal_init(&state, &config) == H2_PAL_ERR_INVALID_ARG);
}

static h2_pal_result_t identity_get(void* user,
                                    h2_pal_device_identity_t* out_identity) {
  *out_identity = *(const h2_pal_device_identity_t*)user;
  return H2_PAL_OK;
}

static h2_pal_result_t mem_get_stats(void* user, h2_pal_mem_region_t region,
                                     h2_pal_mem_stats_t* out_stats) {
  const int fail_external = *(const int*)user;
  out_stats->total_bytes = 1000u + (size_t)region;
  out_stats->free_bytes = 500u;
  out_stats->minimum_free_bytes = 250u;
  out_stats->largest_free_block_bytes = 300u;
  if (fail_external == 2) {
    out_stats->largest_free_block_bytes = 0u;
    out_stats->unavailable_fields =
        H2_PAL_MEM_STATS_UNAVAILABLE_LARGEST_FREE_BLOCK_BYTES;
  }
  return fail_external == 1 && region == H2_PAL_MEM_REGION_EXTERNAL
             ? H2_PAL_ERR_UNSUPPORTED
             : H2_PAL_OK;
}

static void check_observations(void) {
  h2_pal_device_identity_t identity = {
      .bytes = {0u, 1u, 0x0au, 0xbcu, 0xdeu, 0xffu}, .size = 6u};
  static const h2_pal_device_identity_vtable_t identity_vtable = {
      .get = identity_get};
  const h2_pal_device_identity_api_t api = {.user = &identity,
                                            .vtable = &identity_vtable};
  char uid[H2_LOADER_DEVICE_UID_MAX];
  assert(h2_loader_pal_device_uid(&api, uid) == H2_PAL_OK);
  assert(strcmp(uid, "00010abcdeff") == 0);
  identity.size = 16u;
  assert(h2_loader_pal_device_uid(&api, uid) == H2_PAL_ERR_UNSUPPORTED);
  assert(uid[0] == '\0');
  int fail_external = 0;
  static const h2_pal_mem_vtable_t mem_vtable = {.get_stats = mem_get_stats};
  h2_pal_mem_api_t mem = {.user = &fail_external, .vtable = &mem_vtable};
  h2_loader_memory_stats_t stats;
  assert(h2_loader_pal_memory_stats_read(&mem, &stats) == H2_PAL_OK);
  assert(stats.internal.total_bytes == 1000u &&
         stats.iram.total_bytes == 1001u && stats.psram.total_bytes == 1002u);
  assert(stats.psram.free_bytes == 500u &&
         stats.psram.minimum_free_bytes == 250u &&
         stats.psram.largest_free_block_bytes == 300u);
  fail_external = 1;
  assert(h2_loader_pal_memory_stats_read(&mem, &stats) ==
         H2_PAL_ERR_UNSUPPORTED);
  const h2_loader_memory_stats_t empty_stats = {0};
  assert(memcmp(&stats, &empty_stats, sizeof(stats)) == 0);
  fail_external = 2;
  assert(h2_loader_pal_memory_stats_read(&mem, &stats) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(memcmp(&stats, &empty_stats, sizeof(stats)) == 0);
}

int main(void) {
  check_adapters();
  check_observations();
  return 0;
}
