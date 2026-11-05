#include "h2_loader_pal.h"

#include <string.h>

static int image_capacity(void* user, uint32_t partition_id,
                          uint64_t* out_capacity) {
  h2_loader_pal_t* state = user;
  h2_pal_image_info_t info;
  if (out_capacity == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_capacity = 0u;
  int rc = h2_pal_image_get_info(state->config.image, partition_id,
                                 H2_PAL_IMAGE_VIEW_STORED, &info);
  if (rc == H2_PAL_OK) *out_capacity = info.capacity;
  return rc;
}

static int image_read(void* user, uint32_t partition_id, uint64_t offset,
                      void* data, size_t len) {
  h2_loader_pal_t* state = user;
  return h2_pal_image_read(state->config.image, partition_id,
                           H2_PAL_IMAGE_VIEW_STORED, offset, data, len);
}

static uint32_t execution_partition(
    const h2_loader_pal_t* state, const h2_loader_image_identity_t* identity) {
  if (identity == NULL) return 0u;
  if (identity->role == H2_LOADER_IMAGE_ROLE_H2LOADER)
    return state->config.loader_partition_id;
  if (identity->role == H2_LOADER_IMAGE_ROLE_APP)
    return state->config.app_partition_id;
  return 0u;
}

static int image_begin(void* user, uint32_t partition_id,
                       const h2_loader_image_identity_t* identity) {
  h2_loader_pal_t* state = user;
  const uint32_t execution_id = execution_partition(state, identity);
  if (execution_id == 0u || identity->image_size == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (state->last_abort_result != H2_PAL_OK) return state->last_abort_result;
  if (state->writing) return H2_PAL_ERR_BUSY;
  const h2_pal_image_write_request_t request = {
      .partition_id = partition_id,
      .execution_partition_id = execution_id,
      .image_size = identity->image_size,
  };
  int rc = h2_pal_image_begin(state->config.image, &request);
  if (rc == H2_PAL_OK) {
    state->image_size = identity->image_size;
    state->written = 0u;
    state->execution_partition_id = execution_id;
    state->writing = 1;
  }
  return rc;
}

static int image_write(void* user, const void* data, size_t len) {
  h2_loader_pal_t* state = user;
  if (data == NULL && len != 0u) return H2_PAL_ERR_INVALID_ARG;
  if (!state->writing) return H2_PAL_ERR_INVALID_STATE;
  if ((uint64_t)len > state->image_size - state->written)
    return H2_PAL_ERR_NO_SPACE;
  int rc = h2_pal_image_write(state->config.image, data, len);
  if (rc == H2_PAL_OK) state->written += (uint64_t)len;
  return rc;
}

static int image_finish(void* user,
                        const h2_loader_image_identity_t* identity) {
  h2_loader_pal_t* state = user;
  if (identity == NULL || identity->image_size != state->image_size ||
      execution_partition(state, identity) != state->execution_partition_id)
    return H2_PAL_ERR_INVALID_ARG;
  if (!state->writing || state->written != state->image_size)
    return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_pal_image_finish(state->config.image);
  if (rc == H2_PAL_OK) state->writing = 0;
  return rc;
}

static void image_abort(void* user) {
  h2_loader_pal_t* state = user;
  state->last_abort_result = h2_pal_image_abort(state->config.image);
  if (state->last_abort_result == H2_PAL_OK) {
    state->writing = 0;
    state->written = 0u;
    state->image_size = 0u;
    state->execution_partition_id = 0u;
  }
}

static int digest_start(void* user) {
  h2_loader_pal_t* state = user;
  return state->sha_callbacks.start(state->sha_callbacks.user);
}

static int digest_update(void* user, const uint8_t* data, size_t len) {
  h2_loader_pal_t* state = user;
  int rc = state->sha_callbacks.update(state->sha_callbacks.user, data, len);
  if (rc == H2_PAL_OK && len != 0u && state->config.digest_feed_watchdog)
    (void)h2_pal_power_feed_watchdog(state->config.power);
  if (rc == H2_PAL_OK && len != 0u && state->config.digest_yield_ms != 0u)
    rc =
        h2_pal_time_sleep_ms(state->config.time, state->config.digest_yield_ms);
  return rc;
}

static int digest_finish(void* user, uint8_t out_digest[32]) {
  h2_loader_pal_t* state = user;
  return state->sha_callbacks.finish(state->sha_callbacks.user, out_digest);
}

static void digest_abort(void* user) {
  h2_loader_pal_t* state = user;
  state->sha_callbacks.abort(state->sha_callbacks.user);
}

h2_pal_result_t h2_loader_pal_init(h2_loader_pal_t* state,
                                   const h2_loader_pal_config_t* config) {
  if (state == NULL || config == NULL || config->image == NULL ||
      config->power == NULL || config->loader_partition_id == 0u ||
      config->app_partition_id == 0u ||
      (config->identity_view != H2_PAL_IMAGE_VIEW_STORED &&
       config->identity_view != H2_PAL_IMAGE_VIEW_NATIVE) ||
      config->loader_partition_id == config->app_partition_id ||
      (config->digest_yield_ms != 0u &&
       (config->time == NULL || config->time->vtable == NULL ||
        config->time->vtable->sleep_ms == NULL)))
    return H2_PAL_ERR_INVALID_ARG;
  const h2_loader_pal_config_t saved_config = *config;
  memset(state, 0, sizeof(*state));
  state->config = saved_config;
  static const h2_loader_image_reader_vtable_t reader = {
      .get_capacity = image_capacity,
      .read = image_read,
  };
  static const h2_loader_image_writer_vtable_t writer = {
      .get_capacity = image_capacity,
      .begin = image_begin,
      .write = image_write,
      .finish = image_finish,
      .abort = image_abort,
  };
  state->image_reader =
      (h2_loader_image_reader_api_t){.user = state, .vtable = &reader};
  state->image_writer =
      (h2_loader_image_writer_api_t){.user = state, .vtable = &writer};
  h2_pal_result_t rc =
      h2_loader_sha256_digest_init(&state->sha_state, &state->sha_callbacks);
  if (rc != H2_PAL_OK) return rc;
  state->digest = (h2_loader_digest_api_t){
      .user = state,
      .start = digest_start,
      .update = digest_update,
      .finish = digest_finish,
      .abort = digest_abort,
  };
  return H2_PAL_OK;
}

h2_pal_result_t h2_loader_pal_current_image_identity(
    h2_loader_pal_t* state, h2_loader_image_role_t role, const char* board,
    const char* target, const char* version,
    h2_loader_image_identity_t* out_identity) {
  if (out_identity == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_identity, 0, sizeof(*out_identity));
  if (state == NULL || state->config.allocator == NULL ||
      (role != H2_LOADER_IMAGE_ROLE_APP &&
       role != H2_LOADER_IMAGE_ROLE_H2LOADER) ||
      board == NULL || target == NULL || version == NULL || board[0] == '\0' ||
      target[0] == '\0' || version[0] == '\0' ||
      strlen(board) >= sizeof(out_identity->board) ||
      strlen(target) >= sizeof(out_identity->target) ||
      strlen(version) >= sizeof(out_identity->version))
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_power_boot_partition_t running;
  h2_pal_result_t rc =
      h2_pal_power_get_running_boot_partition(state->config.power, &running);
  if (rc != H2_PAL_OK) return rc;
  h2_pal_image_info_t info;
  rc = h2_pal_image_get_info(state->config.image, running.id,
                             state->config.identity_view, &info);
  if (rc != H2_PAL_OK) return rc;
  if ((info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN) == 0u)
    return H2_PAL_ERR_UNSUPPORTED;
  if (info.image_size == 0u) return H2_PAL_ERR_FORMAT;
  uint8_t* buffer = h2_pal_mem_alloc(state->config.allocator, 4096u);
  if (buffer == NULL) return H2_PAL_ERR_NO_MEMORY;
  uint8_t hash[32];
  rc = state->digest.start(state->digest.user);
  for (uint64_t offset = 0u; rc == H2_PAL_OK && offset < info.image_size;) {
    const uint64_t remaining = info.image_size - offset;
    const size_t take = remaining > 4096u ? 4096u : (size_t)remaining;
    rc = h2_pal_image_read(state->config.image, running.id,
                           state->config.identity_view, offset, buffer, take);
    if (rc == H2_PAL_OK)
      rc = state->digest.update(state->digest.user, buffer, take);
    offset += (uint64_t)take;
  }
  if (rc == H2_PAL_OK) rc = state->digest.finish(state->digest.user, hash);
  if (rc != H2_PAL_OK) state->digest.abort(state->digest.user);
  h2_pal_mem_free(state->config.allocator, buffer);
  if (rc != H2_PAL_OK) return rc;
  out_identity->format = 1u;
  out_identity->role = role;
  out_identity->image_size = info.image_size;
  memcpy(out_identity->board, board, strlen(board) + 1u);
  memcpy(out_identity->target, target, strlen(target) + 1u);
  memcpy(out_identity->version, version, strlen(version) + 1u);
  h2_loader_sha256_hex(hash, out_identity->image_sha256);
  return H2_PAL_OK;
}

h2_pal_result_t h2_loader_pal_device_uid(
    const h2_pal_device_identity_api_t* api,
    char out_uid[H2_LOADER_DEVICE_UID_MAX]) {
  if (out_uid == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_uid, 0, H2_LOADER_DEVICE_UID_MAX);
  h2_pal_device_identity_t identity;
  h2_pal_result_t rc = h2_pal_device_identity_get(api, &identity);
  if (rc != H2_PAL_OK) return rc;
  if (identity.size != 6u) return H2_PAL_ERR_UNSUPPORTED;
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0u; i < identity.size; ++i) {
    out_uid[2u * i] = hex[identity.bytes[i] >> 4u];
    out_uid[2u * i + 1u] = hex[identity.bytes[i] & 15u];
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_loader_pal_memory_stats_read(
    void* user, h2_loader_memory_stats_t* out_stats) {
  const h2_pal_mem_api_t* mem = user;
  if (out_stats == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_stats, 0, sizeof(*out_stats));
  const h2_pal_mem_region_t regions[] = {H2_PAL_MEM_REGION_INTERNAL,
                                         H2_PAL_MEM_REGION_INTERNAL_EXECUTABLE,
                                         H2_PAL_MEM_REGION_EXTERNAL};
  h2_loader_memory_stats_t result = {0};
  h2_loader_memory_region_stats_t* destinations[] = {
      &result.internal, &result.iram, &result.psram};
  for (size_t i = 0u; i < sizeof(regions) / sizeof(regions[0]); ++i) {
    h2_pal_mem_stats_t stats;
    h2_pal_result_t rc = h2_pal_mem_get_stats(mem, regions[i], &stats);
    if (rc != H2_PAL_OK) return rc;
    *destinations[i] = (h2_loader_memory_region_stats_t){
        .total_bytes = stats.total_bytes,
        .free_bytes = stats.free_bytes,
        .minimum_free_bytes = stats.minimum_free_bytes,
        .largest_free_block_bytes = stats.largest_free_block_bytes,
    };
  }
  *out_stats = result;
  return H2_PAL_OK;
}

int h2_loader_pal_prepare_file_point(void* user, const char* path) {
  return h2_pal_fs_prepare(user, path);
}

int h2_loader_pal_confirm_running_image(void* user) {
  const h2_pal_power_api_t* power = user;
  h2_pal_power_image_confirmation_t confirmation;
  return h2_pal_power_confirm_running_image(power, NULL, &confirmation);
}
