#include "recording.h"

#if defined(H2_PASSPORT_RECORDING_HOST_TEST)
#include "recording_sdk.h"
#else
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#endif

#include <string.h>

/* Only the verified unused tail of the inactive Loader candidate is borrowed.
 * The canonical 3 MiB App partition and all image bytes remain unchanged. */
enum {
  PARTITION_ID = 2,
  PARTITION_SIZE = 0x300000,
  SCRATCH_OFFSET = 0x2c0000,
  SCRATCH_CAPACITY = 0x40000,
  BLOCK_SIZE = 4096,
  MAX_SAMPLES = 512,
  MAX_CIPHERTEXT = MAX_SAMPLES * 2 + H2_PAL_CRYPTO_AEAD_TAG_SIZE,
};

typedef struct recording_validation {
  h2_runtime_t* runtime;
  h2_loader_t* loader;
  const h2_pal_disk_api_t* disk_api;
  void* disk_user;
  const h2_pal_disk_vtable_t* disk_vtable;
  h2_loader_metadata_t partition_2;
  h2_pal_disk_partition_t disk;
  uint32_t loader_partition_id, app_partition_id, running_partition_id;
  uint32_t app_address, running_address, running_size;
} recording_validation_t;

typedef struct recording_state {
  h2_runtime_t* runtime;
  h2_loader_t* loader;
  recording_validation_t prepared;
  int prepared_valid;
  unsigned count, frames, samples, stride, region_size;
  unsigned written[2];
  int opened, key_live, ready[2];
  uint8_t key[16], nonce_prefix[2][8], ciphertext[MAX_CIPHERTEXT];
} recording_state_t;

static recording_state_t state;

static void clear_bytes(void* data, size_t length) {
  volatile uint8_t* bytes = data;
  while (length-- != 0) *bytes++ = 0;
}

void h2_passport_recording_bind(h2_runtime_t* runtime, h2_loader_t* loader) {
  state.prepared_valid = 0;
  state.runtime = runtime;
  state.loader = loader;
}

static int inspect_region(recording_state_t* s, size_t stride,
                          recording_validation_t* validation) {
  if (s->runtime == NULL || s->loader == NULL ||
      s->loader->status.running_partition_id == PARTITION_ID ||
      s->loader->status.running_partition_id !=
          s->loader->config.h2loader_partition_id ||
      s->loader->config.app_partition_id != PARTITION_ID ||
      s->loader->status.stage.valid || !s->loader->status.partition_2.valid ||
      s->loader->status.partition_2.role != H2_LOADER_IMAGE_ROLE_H2LOADER ||
      s->loader->status.partition_2.image_size > SCRATCH_OFFSET)
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_disk_partition_t disk = {0};
  int rc = h2_pal_disk_get_partition(s->runtime->disk, PARTITION_ID, &disk);
  if (rc != 0) return rc;
  uint32_t flags = H2_PAL_DISK_PARTITION_FLAG_READABLE |
                   H2_PAL_DISK_PARTITION_FLAG_WRITABLE |
                   H2_PAL_DISK_PARTITION_FLAG_ERASABLE;
  if (disk.id != PARTITION_ID || disk.size != PARTITION_SIZE ||
      disk.erase_block_size != BLOCK_SIZE || disk.write_alignment == 0 ||
      BLOCK_SIZE % disk.write_alignment != 0 ||
      stride % disk.write_alignment != 0 || (disk.flags & flags) != flags ||
      memcmp(disk.name, "app", sizeof("app")) != 0)
    return H2_PAL_ERR_FORMAT;
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "app");
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (part == NULL || running == NULL || part->size != PARTITION_SIZE ||
      running->address == part->address)
    return H2_PAL_ERR_INVALID_STATE;
  *validation = (recording_validation_t){
      .runtime = s->runtime,
      .loader = s->loader,
      .disk_api = s->runtime->disk,
      .disk_user = s->runtime->disk->user,
      .disk_vtable = s->runtime->disk->vtable,
      .partition_2 = s->loader->status.partition_2,
      .disk = disk,
      .loader_partition_id = s->loader->config.h2loader_partition_id,
      .app_partition_id = s->loader->config.app_partition_id,
      .running_partition_id = s->loader->status.running_partition_id,
      .app_address = part->address,
      .running_address = running->address,
      .running_size = running->size,
  };
  return 0;
}

static int verify_image(const recording_validation_t* validation) {
  const esp_partition_pos_t position = {.offset = validation->app_address,
                                        .size = PARTITION_SIZE};
  esp_image_metadata_t image = {.start_addr = validation->app_address};
  if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &image) != ESP_OK ||
      image.image_len == 0 || image.image_len > SCRATCH_OFFSET)
    return H2_PAL_ERR_FORMAT;
  return 0;
}

static int same_validation(const recording_validation_t* a,
                           const recording_validation_t* b) {
  const h2_loader_metadata_t* x = &a->partition_2;
  const h2_loader_metadata_t* y = &b->partition_2;
  return a->runtime == b->runtime && a->loader == b->loader &&
         a->disk_api == b->disk_api && a->disk_user == b->disk_user &&
         a->disk_vtable == b->disk_vtable &&
         a->loader_partition_id == b->loader_partition_id &&
         a->app_partition_id == b->app_partition_id &&
         a->running_partition_id == b->running_partition_id &&
         a->app_address == b->app_address &&
         a->running_address == b->running_address &&
         a->running_size == b->running_size &&
         a->disk.id == b->disk.id && a->disk.size == b->disk.size &&
         a->disk.erase_block_size == b->disk.erase_block_size &&
         a->disk.write_alignment == b->disk.write_alignment &&
         a->disk.flags == b->disk.flags &&
         memcmp(a->disk.name, b->disk.name, sizeof(a->disk.name)) == 0 &&
         x->valid == y->valid && x->role == y->role &&
         x->image_size == y->image_size && x->package_size == y->package_size &&
         memcmp(x->image_checksum, y->image_checksum,
                sizeof(x->image_checksum)) == 0 &&
         memcmp(x->package_checksum, y->package_checksum,
                sizeof(x->package_checksum)) == 0 &&
         memcmp(x->version, y->version, sizeof(x->version)) == 0 &&
         memcmp(x->board, y->board, sizeof(x->board)) == 0 &&
         memcmp(x->target, y->target, sizeof(x->target)) == 0;
}

int h2_passport_recording_prepare(void) {
  state.prepared_valid = 0;
  if (state.opened) return H2_PAL_ERR_INVALID_STATE;
  int rc = inspect_region(&state, 0, &state.prepared);
  if (rc == 0) rc = verify_image(&state.prepared);
  if (rc == 0) state.prepared_valid = 1;
  return rc;
}

static int open_recording(void* user, unsigned count, unsigned frames,
                           unsigned samples) {
  recording_state_t* s = user;
  /* prepare and open belong to one uninterrupted Loader operation. Even a
   * rejected open consumes its permit; it cannot survive a later retry. */
  int prepared = s->prepared_valid;
  s->prepared_valid = 0;
  if (s->opened) return H2_PAL_ERR_INVALID_STATE;
  if (count == 0 || count > 2 || frames == 0 || samples == 0 ||
      samples > MAX_SAMPLES || samples % 8 != 0)
    return H2_PAL_ERR_INVALID_ARG;
  size_t stride = samples * sizeof(int16_t) + H2_PAL_CRYPTO_AEAD_TAG_SIZE;
  if (frames > SCRATCH_CAPACITY / stride) return H2_PAL_ERR_NO_SPACE;
  size_t region = (frames * stride + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE;
  if (region > SCRATCH_CAPACITY / count) return H2_PAL_ERR_NO_SPACE;
  recording_validation_t current;
  int rc = inspect_region(s, stride, &current);
  if (rc != 0) return rc;
  if (!prepared || !same_validation(&current, &s->prepared)) {
    rc = verify_image(&current);
    if (rc != 0) return rc;
  }
  s->count = count;
  s->frames = frames;
  s->samples = samples;
  s->stride = (unsigned)stride;
  s->region_size = (unsigned)region;
  s->opened = 1;
  s->key_live = 0;
  memset(s->ready, 0, sizeof(s->ready));
  memset(s->written, 0, sizeof(s->written));
  rc = h2_pal_crypto_random(s->runtime->crypto, s->key, sizeof(s->key));
  if (rc == 0) s->key_live = 1;
  return rc;
}

static int begin_recording(void* user, unsigned recording) {
  recording_state_t* s = user;
  if (!s->opened || !s->key_live || recording >= s->count)
    return H2_PAL_ERR_INVALID_STATE;
  s->ready[recording] = 0;
  s->written[recording] = 0;
  /* Each retry gets a fresh nonce prefix and an erased recording region. */
  int rc = h2_pal_crypto_random(s->runtime->crypto, s->nonce_prefix[recording], 8);
  if (rc == 0)
    rc = h2_pal_disk_erase(s->runtime->disk, PARTITION_ID,
                            SCRATCH_OFFSET + recording * s->region_size,
                            s->region_size);
  if (rc == 0) s->ready[recording] = 1;
  return rc;
}

static void le32(uint8_t* bytes, unsigned value) {
  for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
}

static void frame_nonce(recording_state_t* s, unsigned recording,
                         unsigned frame, uint8_t nonce[12], uint8_t aad[8]) {
  memcpy(nonce, s->nonce_prefix[recording], 8);
  le32(nonce + 8, frame);
  le32(aad, recording);
  le32(aad + 4, frame);
}

static int write_frame(void* user, unsigned recording, unsigned frame,
                       const int16_t* pcm, unsigned samples) {
  recording_state_t* s = user;
  if (!s->opened || !s->key_live || recording >= s->count ||
      !s->ready[recording] || frame >= s->frames ||
      frame != s->written[recording] || samples != s->samples || pcm == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t nonce[12], aad[8];
  frame_nonce(s, recording, frame, nonce, aad);
  h2_pal_crypto_buf_t ciphertext = {
      .data = s->ciphertext, .cap = sizeof(s->ciphertext)};
  int rc = h2_pal_crypto_aead_seal(
      s->runtime->crypto, H2_PAL_CRYPTO_AEAD_AES_128_GCM, s->key, sizeof(s->key),
      nonce, sizeof(nonce), (const uint8_t*)pcm, samples * sizeof(*pcm), aad,
      sizeof(aad), &ciphertext);
  if (rc == 0 && ciphertext.len != s->stride) rc = H2_PAL_ERR_FORMAT;
  if (rc == 0)
    rc = h2_pal_disk_write(s->runtime->disk, PARTITION_ID,
                            SCRATCH_OFFSET + recording * s->region_size +
                                frame * s->stride,
                            s->ciphertext, ciphertext.len);
  clear_bytes(s->ciphertext, sizeof(s->ciphertext));
  if (rc == 0)
    ++s->written[recording];
  else
    /* A partial write must never allow a different frame to reuse its nonce. */
    s->ready[recording] = 0;
  return rc;
}

static int read_frame(void* user, unsigned recording, unsigned frame,
                      int16_t* pcm, unsigned samples) {
  recording_state_t* s = user;
  if (!s->opened || !s->key_live || recording >= s->count ||
      !s->ready[recording] || frame >= s->written[recording] ||
      samples != s->samples || pcm == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_pal_disk_read(s->runtime->disk, PARTITION_ID,
                            SCRATCH_OFFSET + recording * s->region_size +
                                frame * s->stride,
                            s->ciphertext, s->stride);
  uint8_t nonce[12], aad[8];
  frame_nonce(s, recording, frame, nonce, aad);
  h2_pal_crypto_buf_t plaintext = {
      .data = (uint8_t*)pcm, .cap = samples * sizeof(*pcm)};
  if (rc == 0)
    rc = h2_pal_crypto_aead_open(
        s->runtime->crypto, H2_PAL_CRYPTO_AEAD_AES_128_GCM, s->key, sizeof(s->key),
        nonce, sizeof(nonce), s->ciphertext, s->stride, aad, sizeof(aad), &plaintext);
  if (rc == 0 && plaintext.len != samples * sizeof(*pcm))
    rc = H2_PAL_ERR_FORMAT;
  clear_bytes(s->ciphertext, sizeof(s->ciphertext));
  if (rc != 0) clear_bytes(pcm, samples * sizeof(*pcm));
  return rc;
}

static int close_recording(void* user) {
  recording_state_t* s = user;
  s->prepared_valid = 0;
  clear_bytes(s->key, sizeof(s->key));
  clear_bytes(s->nonce_prefix, sizeof(s->nonce_prefix));
  clear_bytes(s->ciphertext, sizeof(s->ciphertext));
  s->key_live = 0;
  memset(s->ready, 0, sizeof(s->ready));
  if (!s->opened) return 0;
  int rc = h2_pal_disk_erase(s->runtime->disk, PARTITION_ID, SCRATCH_OFFSET,
                              s->count * s->region_size);
  if (rc != 0) return rc;
  s->opened = 0;
  memset(s->written, 0, sizeof(s->written));
  return 0;
}

const h2_mfg_recording_store_t h2_passport_recording_store = {
    .user = &state,
    .open = open_recording,
    .begin = begin_recording,
    .write_frame = write_frame,
    .read_frame = read_frame,
    .close = close_recording};
