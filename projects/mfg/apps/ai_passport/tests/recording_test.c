#include "recording.h"
#include "recording_sdk.h"
#include "h2_wolfcrypt_crypto.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  FLASH_SIZE = 0x800000,
  APP_ADDRESS = 0x220000,
  APP_SIZE = 0x300000,
  SCRATCH_OFFSET = 0x2c0000,
  SCRATCH_SIZE = 0x40000,
  BLOCK_SIZE = 4096,
  PARTITION_ID = 2,
  MAX_SAMPLES = 512,
  FRAME_COUNT = 96,
  MAX_CIPHERTEXT = MAX_SAMPLES * 2 + H2_PAL_CRYPTO_AEAD_TAG_SIZE,
};

static const char* test_name;
#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, test_name,         \
              #expression);                                                   \
      exit(1);                                                                \
    }                                                                         \
  } while (0)

static const h2_mfg_recording_store_t* const store =
    &h2_passport_recording_store;

typedef struct test_entropy {
  uint32_t state;
  unsigned calls, fail_on;
  uint8_t* key_address;
  uint8_t key[16];
  uint8_t* nonce_address[16];
  uint8_t nonce[16][8];
  unsigned nonce_count;
} test_entropy_t;

typedef struct fixture {
  h2_runtime_t runtime;
  h2_loader_t loader;
  h2_pal_disk_partition_t partition;
  h2_pal_disk_api_t disk;
  test_entropy_t entropy;
  esp_partition_t app, running;
  int app_missing, running_missing, verify_result, get_result;
  uint32_t image_length;
  unsigned get_calls, image_calls, read_calls, write_calls, erase_calls;
  unsigned fail_read_on, fail_write_on, fail_erase_on;
  uint64_t last_erase_offset, last_erase_length;
  uint8_t flash[FLASH_SIZE], before[FLASH_SIZE];
} fixture_t;

static fixture_t fixture;

static int entropy_fill(void* user, uint8_t* out, size_t length) {
  test_entropy_t* entropy = user;
  ++entropy->calls;
  if (length == sizeof(entropy->key)) entropy->key_address = out;
  for (size_t i = 0; i < length; ++i) {
    entropy->state = entropy->state * UINT32_C(1664525) + UINT32_C(1013904223);
    out[i] = (uint8_t)(entropy->state >> 24);
    /* Exercise a source that has already produced partial output on failure. */
    if (entropy->calls == entropy->fail_on && i == length / 2)
      return H2_PAL_ERR_IO;
  }
  if (length == sizeof(entropy->key)) {
    memcpy(entropy->key, out, length);
  } else {
    CHECK(length == sizeof(entropy->nonce[0]));
    CHECK(entropy->nonce_count < 16);
    entropy->nonce_address[entropy->nonce_count] = out;
    memcpy(entropy->nonce[entropy->nonce_count++], out, length);
  }
  return 0;
}

const esp_partition_t* esp_partition_find_first(
    esp_partition_type_t type, esp_partition_subtype_t subtype,
    const char* label) {
  CHECK(type == ESP_PARTITION_TYPE_APP);
  CHECK(subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1);
  CHECK(strcmp(label, "app") == 0);
  return fixture.app_missing ? NULL : &fixture.app;
}

const esp_partition_t* esp_ota_get_running_partition(void) {
  return fixture.running_missing ? NULL : &fixture.running;
}

esp_err_t esp_image_verify(esp_image_load_mode_t mode,
                           const esp_partition_pos_t* position,
                           esp_image_metadata_t* metadata) {
  CHECK(mode == ESP_IMAGE_VERIFY_SILENT);
  CHECK(position->offset == fixture.app.address);
  CHECK(position->size == fixture.app.size);
  CHECK(metadata->start_addr == fixture.app.address);
  ++fixture.image_calls;
  metadata->image_len = fixture.image_length;
  return fixture.verify_result;
}

static int disk_get(void* user, uint32_t id,
                    h2_pal_disk_partition_t* partition) {
  fixture_t* f = user;
  CHECK(id == PARTITION_ID);
  ++f->get_calls;
  if (f->get_result != 0) return f->get_result;
  *partition = f->partition;
  return 0;
}

static uint8_t* disk_range(fixture_t* f, uint32_t id, uint64_t offset,
                           uint64_t length) {
  CHECK(id == PARTITION_ID);
  CHECK(offset <= APP_SIZE && length <= APP_SIZE - offset);
  return f->flash + APP_ADDRESS + (size_t)offset;
}

static int disk_read(void* user, uint32_t id, uint64_t offset, void* data,
                     size_t length) {
  fixture_t* f = user;
  uint8_t* source = disk_range(f, id, offset, length);
  ++f->read_calls;
  if (f->read_calls == f->fail_read_on) {
    memcpy(data, source, length / 2);
    return H2_PAL_ERR_IO;
  }
  memcpy(data, source, length);
  return 0;
}

static int disk_write(void* user, uint32_t id, uint64_t offset,
                      const void* data, size_t length) {
  fixture_t* f = user;
  uint8_t* destination = disk_range(f, id, offset, length);
  const uint8_t* source = data;
  CHECK(offset >= SCRATCH_OFFSET);
  CHECK(f->partition.write_alignment != 0);
  CHECK(offset % f->partition.write_alignment == 0);
  CHECK(length % f->partition.write_alignment == 0);
  ++f->write_calls;
  int fail = f->write_calls == f->fail_write_on;
  size_t written = fail ? length / 2 : length;
  for (size_t i = 0; i < written; ++i) {
    /* Real NOR programming can only change erased bits from 1 to 0. */
    CHECK((destination[i] & source[i]) == source[i]);
    destination[i] = source[i];
  }
  return fail ? H2_PAL_ERR_IO : 0;
}

static int disk_erase(void* user, uint32_t id, uint64_t offset,
                      uint64_t length) {
  fixture_t* f = user;
  uint8_t* destination = disk_range(f, id, offset, length);
  CHECK(offset >= SCRATCH_OFFSET);
  CHECK(offset % BLOCK_SIZE == 0 && length % BLOCK_SIZE == 0);
  ++f->erase_calls;
  f->last_erase_offset = offset;
  f->last_erase_length = length;
  if (f->erase_calls == f->fail_erase_on) {
    memset(destination, 0xff, BLOCK_SIZE);
    return H2_PAL_ERR_IO;
  }
  memset(destination, 0xff, (size_t)length);
  return 0;
}

static const h2_pal_disk_vtable_t disk_vtable = {
    .get_partition = disk_get,
    .read = disk_read,
    .erase = disk_erase,
    .write = disk_write,
};

static void reset_fixture(void) {
  CHECK(store->close(store->user) == 0);
  memset(&fixture, 0, sizeof(fixture));
  fixture.entropy.state = UINT32_C(0xf0cacc1a);
  fixture.partition = (h2_pal_disk_partition_t){
      .id = PARTITION_ID,
      .size = APP_SIZE,
      .erase_block_size = BLOCK_SIZE,
      .write_alignment = 1,
      .flags = H2_PAL_DISK_PARTITION_FLAG_READABLE |
               H2_PAL_DISK_PARTITION_FLAG_WRITABLE |
               H2_PAL_DISK_PARTITION_FLAG_ERASABLE |
               H2_PAL_DISK_PARTITION_FLAG_BOOTABLE,
      .name = "app",
  };
  fixture.disk = (h2_pal_disk_api_t){.user = &fixture, .vtable = &disk_vtable};
  fixture.runtime.disk = &fixture.disk;
  fixture.runtime.crypto = h2_wolfcrypt_crypto_api();
  fixture.loader.config.h2loader_partition_id = 1;
  fixture.loader.config.app_partition_id = PARTITION_ID;
  fixture.loader.status.running_partition_id = 1;
  fixture.loader.status.partition_2.valid = 1;
  fixture.loader.status.partition_2.role = H2_LOADER_IMAGE_ROLE_H2LOADER;
  fixture.loader.status.partition_2.image_size = 0x180000;
  fixture.app = (esp_partition_t){
      .type = ESP_PARTITION_TYPE_APP,
      .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
      .address = APP_ADDRESS,
      .size = APP_SIZE,
  };
  fixture.running.address = 0x20000;
  fixture.running.size = 0x200000;
  fixture.image_length = 0x180000;
  for (size_t i = 0; i < sizeof(fixture.flash); ++i)
    fixture.flash[i] = (uint8_t)((i * 13u + i / BLOCK_SIZE) & 0xffu);
  memcpy(fixture.before, fixture.flash, sizeof(fixture.flash));
  h2_passport_recording_bind(&fixture.runtime, &fixture.loader);
}

static unsigned frame_stride(unsigned samples) {
  return samples * 2u + H2_PAL_CRYPTO_AEAD_TAG_SIZE;
}

static unsigned region_size(unsigned frames, unsigned samples) {
  return (frames * frame_stride(samples) + BLOCK_SIZE - 1) / BLOCK_SIZE *
         BLOCK_SIZE;
}

static uint8_t* frame_bytes(unsigned recording, unsigned frame,
                            unsigned frames, unsigned samples) {
  return fixture.flash + APP_ADDRESS + SCRATCH_OFFSET +
         recording * region_size(frames, samples) + frame * frame_stride(samples);
}

static void check_bytes(const void* data, size_t length, uint8_t value) {
  const uint8_t* bytes = data;
  for (size_t i = 0; i < length; ++i) CHECK(bytes[i] == value);
}

static void check_outside_recordings(unsigned count, unsigned frames,
                                     unsigned samples) {
  size_t start = APP_ADDRESS + SCRATCH_OFFSET;
  size_t end = start + count * region_size(frames, samples);
  CHECK(end <= APP_ADDRESS + APP_SIZE);
  CHECK(memcmp(fixture.flash, fixture.before, start) == 0);
  CHECK(memcmp(fixture.flash + end, fixture.before + end, FLASH_SIZE - end) == 0);
}

static void fill_pcm(int16_t* pcm, unsigned samples, unsigned recording,
                      unsigned frame) {
  for (unsigned i = 0; i < samples; ++i)
    pcm[i] = (int16_t)((int)((i * 73u + frame * 997u + recording * 13007u) &
                            0xffffu) - 32768);
}

static void check_read(unsigned recording, unsigned frame, unsigned samples) {
  int16_t expected[MAX_SAMPLES], actual[MAX_SAMPLES];
  fill_pcm(expected, samples, recording, frame);
  memset(actual, 0xa5, sizeof(actual));
  CHECK(store->read_frame(store->user, recording, frame, actual, samples) == 0);
  CHECK(memcmp(actual, expected, samples * sizeof(*actual)) == 0);
  check_bytes(actual + samples, sizeof(actual) - samples * sizeof(*actual), 0xa5);
}

static void check_read_failure(unsigned recording, unsigned frame,
                                unsigned samples) {
  int16_t output[MAX_SAMPLES];
  memset(output, 0xa5, sizeof(output));
  CHECK(store->read_frame(store->user, recording, frame, output, samples) != 0);
  check_bytes(output, samples * sizeof(*output), 0);
  check_bytes(output + samples, sizeof(output) - samples * sizeof(*output), 0xa5);
}

static void write_pcm(unsigned recording, unsigned frame, unsigned samples) {
  int16_t pcm[MAX_SAMPLES];
  fill_pcm(pcm, samples, recording, frame);
  CHECK(store->write_frame(store->user, recording, frame, pcm, samples) == 0);
}

static void check_no_flash_changes(void) {
  CHECK(fixture.erase_calls == 0);
  CHECK(fixture.write_calls == 0);
  CHECK(memcmp(fixture.flash, fixture.before, FLASH_SIZE) == 0);
}

static void test_open_guards(void) {
  enum guard {
    NO_RUNTIME, NO_LOADER, RUNNING_P2, LOADER_IS_P2, RUNNING_UNKNOWN,
    APP_ID, STAGE_VALID, P2_INVALID, P2_APP, P2_ROLE_UNKNOWN, P2_OVERLAP,
    NO_DISK, DISK_QUERY, DISK_ID, DISK_SMALL, DISK_LARGE, DISK_ERASE_SIZE,
    DISK_NO_READ, DISK_NO_WRITE, DISK_NO_ERASE, DISK_NAME, DISK_ALIGNMENT_ZERO,
    DISK_ALIGNMENT_ODD, DISK_ALIGNMENT_TOO_LARGE, NO_PHYSICAL_APP,
    NO_PHYSICAL_RUNNING, PHYSICAL_SMALL, PHYSICAL_LARGE, PHYSICAL_RUNNING_P2,
    IMAGE_INVALID, IMAGE_EMPTY, IMAGE_OVERLAP, GUARD_COUNT,
  };
  static const char* const names[GUARD_COUNT] = {
      "no runtime", "no loader", "running P2", "Loader configured as P2",
      "unknown running partition", "wrong app ID", "valid Stage", "invalid P2",
      "P2 app role", "unknown P2 role", "logical image overlap", "no disk",
      "disk query failure", "wrong disk ID", "small disk", "large disk",
      "wrong erase block", "unreadable disk", "unwritable disk",
      "unerasable disk", "wrong disk name", "zero write alignment",
      "odd write alignment", "incompatible write alignment", "no physical app",
      "no physical running app", "small physical app", "large physical app",
      "physical running P2", "invalid physical image", "empty physical image",
      "physical image overlap",
  };
  for (unsigned attempt = 0; attempt < GUARD_COUNT * 2; ++attempt) {
    unsigned guard = attempt % GUARD_COUNT;
    reset_fixture();
    if (attempt >= GUARD_COUNT) CHECK(h2_passport_recording_prepare() == 0);
    test_name = names[guard];
    int error = H2_PAL_ERR_INVALID_STATE;
    switch (guard) {
      case NO_RUNTIME:
        h2_passport_recording_bind(NULL, &fixture.loader);
        break;
      case NO_LOADER:
        h2_passport_recording_bind(&fixture.runtime, NULL);
        break;
      case RUNNING_P2:
        fixture.loader.status.running_partition_id = 2;
        break;
      case LOADER_IS_P2:
        fixture.loader.config.h2loader_partition_id = 2;
        fixture.loader.status.running_partition_id = 2;
        break;
      case RUNNING_UNKNOWN:
        fixture.loader.status.running_partition_id = 0;
        break;
      case APP_ID:
        fixture.loader.config.app_partition_id = 1;
        break;
      case STAGE_VALID:
        fixture.loader.status.stage.valid = 1;
        break;
      case P2_INVALID:
        fixture.loader.status.partition_2.valid = 0;
        break;
      case P2_APP:
        fixture.loader.status.partition_2.role = H2_LOADER_IMAGE_ROLE_APP;
        break;
      case P2_ROLE_UNKNOWN:
        fixture.loader.status.partition_2.role = 0;
        break;
      case P2_OVERLAP:
        fixture.loader.status.partition_2.image_size = UINT64_MAX;
        break;
      case NO_DISK:
        fixture.runtime.disk = NULL;
        error = H2_PAL_ERR_UNSUPPORTED;
        break;
      case DISK_QUERY:
        fixture.get_result = error = H2_PAL_ERR_IO;
        break;
      case DISK_ID:
        fixture.partition.id = 1;
        error = H2_PAL_ERR_FORMAT;
        break;
      case DISK_SMALL:
        --fixture.partition.size;
        error = H2_PAL_ERR_FORMAT;
        break;
      case DISK_LARGE:
        ++fixture.partition.size;
        error = H2_PAL_ERR_FORMAT;
        break;
      case DISK_ERASE_SIZE:
        fixture.partition.erase_block_size = 2048;
        error = H2_PAL_ERR_FORMAT;
        break;
      case DISK_NO_READ:
        fixture.partition.flags &= ~H2_PAL_DISK_PARTITION_FLAG_READABLE;
        error = H2_PAL_ERR_FORMAT;
        break;
      case DISK_NO_WRITE:
        fixture.partition.flags &= ~H2_PAL_DISK_PARTITION_FLAG_WRITABLE;
        error = H2_PAL_ERR_FORMAT;
        break;
      case DISK_NO_ERASE:
        fixture.partition.flags &= ~H2_PAL_DISK_PARTITION_FLAG_ERASABLE;
        error = H2_PAL_ERR_FORMAT;
        break;
      case DISK_NAME:
        memcpy(fixture.partition.name, "nvs", 4);
        error = H2_PAL_ERR_FORMAT;
        break;
      case DISK_ALIGNMENT_ZERO:
        fixture.partition.write_alignment = 0;
        error = H2_PAL_ERR_FORMAT;
        break;
      case DISK_ALIGNMENT_ODD:
        fixture.partition.write_alignment = 3;
        error = H2_PAL_ERR_FORMAT;
        break;
      case DISK_ALIGNMENT_TOO_LARGE:
        fixture.partition.write_alignment = BLOCK_SIZE;
        error = H2_PAL_ERR_FORMAT;
        break;
      case NO_PHYSICAL_APP:
        fixture.app_missing = 1;
        break;
      case NO_PHYSICAL_RUNNING:
        fixture.running_missing = 1;
        break;
      case PHYSICAL_SMALL:
        --fixture.app.size;
        break;
      case PHYSICAL_LARGE:
        ++fixture.app.size;
        break;
      case PHYSICAL_RUNNING_P2:
        fixture.running.address = APP_ADDRESS;
        break;
      case IMAGE_INVALID:
        fixture.verify_result = ESP_FAIL;
        error = H2_PAL_ERR_FORMAT;
        break;
      case IMAGE_EMPTY:
        fixture.image_length = 0;
        error = H2_PAL_ERR_FORMAT;
        break;
      case IMAGE_OVERLAP:
        fixture.image_length = SCRATCH_OFFSET + 1;
        error = H2_PAL_ERR_FORMAT;
        break;
      default:
        CHECK(0);
    }
    /* A replaced physical image also changes its Loader identity. The permit
     * is valid only while one continuous Loader operation excludes replacement. */
    if (guard >= IMAGE_INVALID)
      fixture.loader.status.partition_2.image_checksum[0] = 'a';
    CHECK(store->open(store->user, 1, FRAME_COUNT, 256) == error);
    CHECK(store->begin(store->user, 0) != 0);
    CHECK(store->close(store->user) == 0);
    CHECK(store->close(store->user) == 0);
    CHECK(fixture.entropy.calls == 0);
    check_no_flash_changes();
  }
}

static void test_invalid_dimensions(void) {
  static const struct {
    unsigned count, frames, samples;
    int error;
  } cases[] = {
      {0, FRAME_COUNT, 256, H2_PAL_ERR_INVALID_ARG},
      {3, FRAME_COUNT, 256, H2_PAL_ERR_INVALID_ARG},
      {1, 0, 256, H2_PAL_ERR_INVALID_ARG},
      {1, FRAME_COUNT, 0, H2_PAL_ERR_INVALID_ARG},
      {1, FRAME_COUNT, 7, H2_PAL_ERR_INVALID_ARG},
      {1, FRAME_COUNT, 257, H2_PAL_ERR_INVALID_ARG},
      {1, FRAME_COUNT, 513, H2_PAL_ERR_INVALID_ARG},
      {1, FRAME_COUNT, UINT_MAX, H2_PAL_ERR_INVALID_ARG},
      {1, UINT_MAX, 512, H2_PAL_ERR_NO_SPACE},
      {2, 127, 512, H2_PAL_ERR_NO_SPACE},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    reset_fixture();
    CHECK(store->open(store->user, cases[i].count, cases[i].frames,
                      cases[i].samples) == cases[i].error);
    CHECK(store->close(store->user) == 0);
    CHECK(fixture.get_calls == 0 && fixture.image_calls == 0);
    CHECK(fixture.entropy.calls == 0);
    check_no_flash_changes();
  }
}

static void check_aes_gcm_record(unsigned recording, unsigned samples) {
  uint8_t nonce[12] = {0}, aad[8] = {0};
  int16_t actual[MAX_SAMPLES], expected[MAX_SAMPLES];
  memcpy(nonce, fixture.entropy.nonce[recording], 8);
  aad[0] = (uint8_t)recording;
  h2_pal_crypto_buf_t plaintext = {
      .data = (uint8_t*)actual, .cap = samples * sizeof(*actual)};
  CHECK(h2_pal_crypto_aead_open(
            h2_wolfcrypt_crypto_api(), H2_PAL_CRYPTO_AEAD_AES_128_GCM,
            fixture.entropy.key, sizeof(fixture.entropy.key), nonce,
            sizeof(nonce), frame_bytes(recording, 0, FRAME_COUNT, samples),
            frame_stride(samples), aad, sizeof(aad), &plaintext) == 0);
  CHECK(plaintext.len == samples * sizeof(*actual));
  fill_pcm(expected, samples, recording, 0);
  CHECK(memcmp(actual, expected, plaintext.len) == 0);
}

static void test_full_recordings(void) {
  static const unsigned sample_counts[] = {256, 320, 512};
  for (unsigned count = 1; count <= 2; ++count) {
    for (size_t size = 0; size < sizeof(sample_counts) / sizeof(sample_counts[0]); ++size) {
      unsigned samples = sample_counts[size];
      unsigned region = region_size(FRAME_COUNT, samples);
      reset_fixture();
      /* Equality is safe: the last verified image byte precedes the tail. */
      fixture.image_length = SCRATCH_OFFSET;
      fixture.loader.status.partition_2.image_size = SCRATCH_OFFSET;
      fixture.partition.write_alignment = 16;
      CHECK(store->open(store->user, count, FRAME_COUNT, samples) == 0);
      CHECK(fixture.image_calls == 1 && fixture.entropy.calls == 1);
      CHECK(fixture.erase_calls == 0 && fixture.write_calls == 0);
      for (unsigned recording = 0; recording < count; ++recording) {
        CHECK(store->begin(store->user, recording) == 0);
        CHECK(fixture.last_erase_offset == SCRATCH_OFFSET + recording * region);
        CHECK(fixture.last_erase_length == region);
        check_bytes(frame_bytes(recording, 0, FRAME_COUNT, samples), region, 0xff);
        for (unsigned frame = 0; frame < FRAME_COUNT; ++frame) {
          int16_t pcm[MAX_SAMPLES];
          fill_pcm(pcm, samples, recording, frame);
          CHECK(store->write_frame(store->user, recording, frame, pcm, samples) == 0);
          CHECK(memcmp(frame_bytes(recording, frame, FRAME_COUNT, samples), pcm,
                       samples * sizeof(*pcm)) != 0);
        }
      }
      CHECK(fixture.write_calls == count * FRAME_COUNT);
      for (unsigned recording = 0; recording < count; ++recording) {
        for (unsigned frame = FRAME_COUNT; frame-- != 0;)
          check_read(recording, frame, samples);
        check_read(recording, 0, samples);
        check_aes_gcm_record(recording, samples);
      }
      check_outside_recordings(count, FRAME_COUNT, samples);
      CHECK(store->close(store->user) == 0);
      CHECK(fixture.last_erase_offset == SCRATCH_OFFSET);
      CHECK(fixture.last_erase_length == count * region);
      check_bytes(frame_bytes(0, 0, FRAME_COUNT, samples), count * region, 0xff);
      check_bytes(fixture.entropy.key_address, 16, 0);
      for (unsigned recording = 0; recording < count; ++recording)
        check_bytes(fixture.entropy.nonce_address[recording], 8, 0);
      CHECK(store->close(store->user) == 0);
      CHECK(fixture.erase_calls == count + 1);
      check_outside_recordings(count, FRAME_COUNT, samples);
    }
  }
}

static void test_retry_renews_nonce(void) {
  reset_fixture();
  CHECK(store->open(store->user, 2, FRAME_COUNT, 256) == 0);
  CHECK(store->begin(store->user, 0) == 0);
  CHECK(store->begin(store->user, 1) == 0);
  write_pcm(0, 0, 256);
  write_pcm(1, 0, 256);
  uint8_t old[MAX_CIPHERTEXT], fresh[MAX_CIPHERTEXT], other[MAX_CIPHERTEXT];
  unsigned stride = frame_stride(256);
  memcpy(old, frame_bytes(0, 0, FRAME_COUNT, 256), stride);
  memcpy(other, frame_bytes(1, 0, FRAME_COUNT, 256), stride);
  CHECK(store->begin(store->user, 0) == 0);
  CHECK(fixture.erase_calls == 3);
  CHECK(fixture.last_erase_offset == SCRATCH_OFFSET);
  CHECK(fixture.last_erase_length == region_size(FRAME_COUNT, 256));
  CHECK(memcmp(fixture.entropy.nonce[0], fixture.entropy.nonce[2], 8) != 0);
  CHECK(memcmp(fixture.entropy.key_address, fixture.entropy.key, 16) == 0);
  check_bytes(frame_bytes(0, 0, FRAME_COUNT, 256), region_size(FRAME_COUNT, 256), 0xff);
  CHECK(memcmp(other, frame_bytes(1, 0, FRAME_COUNT, 256), stride) == 0);
  check_read(1, 0, 256);
  int16_t pcm[MAX_SAMPLES];
  CHECK(store->read_frame(store->user, 0, 0, pcm, 256) != 0);
  write_pcm(0, 0, 256);
  memcpy(fresh, frame_bytes(0, 0, FRAME_COUNT, 256), stride);
  CHECK(memcmp(old, fresh, stride) != 0);
  memcpy(frame_bytes(0, 0, FRAME_COUNT, 256), old, stride);
  check_read_failure(0, 0, 256);
  memcpy(frame_bytes(0, 0, FRAME_COUNT, 256), fresh, stride);
  check_read(0, 0, 256);
  CHECK(store->close(store->user) == 0);
  check_outside_recordings(2, FRAME_COUNT, 256);
}

static void test_tamper_and_read_failure(void) {
  reset_fixture();
  CHECK(store->open(store->user, 2, 3, 320) == 0);
  for (unsigned recording = 0; recording < 2; ++recording) {
    CHECK(store->begin(store->user, recording) == 0);
    for (unsigned frame = 0; frame < 3; ++frame) write_pcm(recording, frame, 320);
  }
  uint8_t* ciphertext = frame_bytes(0, 0, 3, 320);
  unsigned stride = frame_stride(320);
  uint8_t saved[MAX_CIPHERTEXT];
  memcpy(saved, ciphertext, stride);
  ciphertext[0] ^= 0x80;
  check_read_failure(0, 0, 320);
  memcpy(ciphertext, saved, stride);
  ciphertext[stride - 1] ^= 1;
  check_read_failure(0, 0, 320);
  memcpy(ciphertext, frame_bytes(0, 1, 3, 320), stride);
  check_read_failure(0, 0, 320);
  memcpy(ciphertext, frame_bytes(1, 0, 3, 320), stride);
  check_read_failure(0, 0, 320);
  memcpy(ciphertext, saved, stride);
  fixture.fail_read_on = fixture.read_calls + 1;
  check_read_failure(0, 0, 320);
  check_read(0, 0, 320);
  CHECK(store->close(store->user) == 0);
  check_outside_recordings(2, 3, 320);
}

static void test_partial_write_requires_begin(void) {
  reset_fixture();
  CHECK(store->open(store->user, 2, 3, 512) == 0);
  CHECK(store->begin(store->user, 0) == 0);
  CHECK(store->begin(store->user, 1) == 0);
  write_pcm(0, 0, 512);
  write_pcm(1, 0, 512);
  fixture.fail_write_on = fixture.write_calls + 1;
  int16_t pcm[MAX_SAMPLES];
  fill_pcm(pcm, 512, 0, 1);
  CHECK(store->write_frame(store->user, 0, 1, pcm, 512) == H2_PAL_ERR_IO);
  CHECK(memcmp(frame_bytes(0, 1, 3, 512), frame_bytes(0, 2, 3, 512),
               frame_stride(512) / 2) != 0);
  check_bytes(frame_bytes(0, 1, 3, 512) + frame_stride(512) / 2,
              frame_stride(512) / 2, 0xff);
  unsigned writes = fixture.write_calls, reads = fixture.read_calls;
  memset(pcm, 0x73, sizeof(pcm));
  CHECK(store->write_frame(store->user, 0, 1, pcm, 512) != 0);
  CHECK(store->write_frame(store->user, 0, 2, pcm, 512) != 0);
  CHECK(store->read_frame(store->user, 0, 0, pcm, 512) != 0);
  CHECK(fixture.write_calls == writes && fixture.read_calls == reads);
  check_read(1, 0, 512);
  CHECK(store->begin(store->user, 0) == 0);
  CHECK(memcmp(fixture.entropy.nonce[0], fixture.entropy.nonce[2], 8) != 0);
  check_bytes(frame_bytes(0, 0, 3, 512), region_size(3, 512), 0xff);
  for (unsigned frame = 0; frame < 3; ++frame) write_pcm(0, frame, 512);
  for (unsigned frame = 0; frame < 3; ++frame) check_read(0, frame, 512);
  CHECK(store->close(store->user) == 0);
  check_outside_recordings(2, 3, 512);
}

static void test_begin_failures_disable_recording(void) {
  reset_fixture();
  CHECK(store->open(store->user, 2, 3, 256) == 0);
  CHECK(store->begin(store->user, 0) == 0);
  CHECK(store->begin(store->user, 1) == 0);
  write_pcm(0, 0, 256);
  write_pcm(1, 0, 256);
  fixture.entropy.fail_on = fixture.entropy.calls + 1;
  CHECK(store->begin(store->user, 0) == H2_PAL_ERR_IO);
  CHECK(fixture.erase_calls == 2);
  int16_t pcm[MAX_SAMPLES] = {0};
  CHECK(store->read_frame(store->user, 0, 0, pcm, 256) != 0);
  CHECK(store->write_frame(store->user, 0, 0, pcm, 256) != 0);
  CHECK(fixture.write_calls == 2 && fixture.read_calls == 0);
  check_read(1, 0, 256);
  fixture.fail_erase_on = fixture.erase_calls + 1;
  CHECK(store->begin(store->user, 0) == H2_PAL_ERR_IO);
  CHECK(store->write_frame(store->user, 0, 0, pcm, 256) != 0);
  CHECK(store->read_frame(store->user, 0, 0, pcm, 256) != 0);
  CHECK(fixture.write_calls == 2 && fixture.read_calls == 1);
  CHECK(store->begin(store->user, 0) == 0);
  CHECK(memcmp(fixture.entropy.nonce[2], fixture.entropy.nonce[3], 8) != 0);
  write_pcm(0, 0, 256);
  check_read(0, 0, 256);
  check_read(1, 0, 256);
  CHECK(store->close(store->user) == 0);
  check_outside_recordings(2, 3, 256);
}

static void test_failed_close_retains_ownership_and_clears_key(void) {
  reset_fixture();
  CHECK(h2_passport_recording_prepare() == 0);
  CHECK(store->open(store->user, 2, FRAME_COUNT, 512) == 0);
  CHECK(fixture.image_calls == 1);
  CHECK(store->begin(store->user, 0) == 0);
  CHECK(store->begin(store->user, 1) == 0);
  write_pcm(0, 0, 512);
  write_pcm(1, 0, 512);
  uint8_t old_key[16];
  memcpy(old_key, fixture.entropy.key, sizeof(old_key));
  fixture.fail_erase_on = fixture.erase_calls + 1;
  CHECK(store->close(store->user) == H2_PAL_ERR_IO);
  check_bytes(fixture.entropy.key_address, 16, 0);
  check_bytes(fixture.entropy.nonce_address[0], 8, 0);
  check_bytes(fixture.entropy.nonce_address[1], 8, 0);
  int16_t pcm[MAX_SAMPLES] = {0};
  unsigned entropy = fixture.entropy.calls;
  CHECK(store->read_frame(store->user, 1, 0, pcm, 512) != 0);
  CHECK(store->write_frame(store->user, 1, 1, pcm, 512) != 0);
  CHECK(store->begin(store->user, 1) != 0);
  CHECK(store->open(store->user, 1, FRAME_COUNT, 256) == H2_PAL_ERR_INVALID_STATE);
  CHECK(fixture.read_calls == 0 && fixture.write_calls == 2);
  CHECK(fixture.entropy.calls == entropy);
  CHECK(store->close(store->user) == 0);
  CHECK(fixture.last_erase_offset == SCRATCH_OFFSET);
  CHECK(fixture.last_erase_length == 2 * region_size(FRAME_COUNT, 512));
  check_bytes(frame_bytes(0, 0, FRAME_COUNT, 512),
              2 * region_size(FRAME_COUNT, 512), 0xff);
  CHECK(store->close(store->user) == 0);
  CHECK(fixture.erase_calls == 4);
  check_outside_recordings(2, FRAME_COUNT, 512);
  CHECK(store->open(store->user, 1, FRAME_COUNT, 256) == 0);
  CHECK(memcmp(old_key, fixture.entropy.key, sizeof(old_key)) != 0);
  CHECK(store->close(store->user) == 0);
}

static void test_failed_open_cleanup(void) {
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    unsigned missing_crypto = attempt % 2;
    reset_fixture();
    if (attempt >= 2) {
      CHECK(h2_passport_recording_prepare() == 0);
      fixture.verify_result = ESP_FAIL;
    }
    if (missing_crypto)
      fixture.runtime.crypto = NULL;
    else
      fixture.entropy.fail_on = 1;
    CHECK(store->open(store->user, 2, FRAME_COUNT, 320) ==
          (missing_crypto ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_ERR_IO));
    check_no_flash_changes();
    CHECK(store->begin(store->user, 0) != 0);
    CHECK(store->open(store->user, 1, FRAME_COUNT, 256) == H2_PAL_ERR_INVALID_STATE);
    fixture.fail_erase_on = 1;
    CHECK(store->close(store->user) == H2_PAL_ERR_IO);
    if (!missing_crypto) check_bytes(fixture.entropy.key_address, 16, 0);
    CHECK(store->open(store->user, 1, FRAME_COUNT, 256) == H2_PAL_ERR_INVALID_STATE);
    CHECK(store->close(store->user) == 0);
    CHECK(fixture.last_erase_offset == SCRATCH_OFFSET);
    CHECK(fixture.last_erase_length == 2 * region_size(FRAME_COUNT, 320));
    CHECK(store->close(store->user) == 0);
    CHECK(fixture.erase_calls == 2 && fixture.write_calls == 0);
    check_outside_recordings(2, FRAME_COUNT, 320);
    fixture.runtime.crypto = h2_wolfcrypt_crypto_api();
    fixture.verify_result = ESP_OK;
    unsigned verifications = fixture.image_calls;
    CHECK(store->open(store->user, 1, FRAME_COUNT, 256) == 0);
    CHECK(fixture.image_calls == verifications + 1);
    CHECK(store->begin(store->user, 0) == 0);
    write_pcm(0, 0, 256);
    check_read(0, 0, 256);
    CHECK(store->close(store->user) == 0);
  }
}

static void test_prepare_verifies_once(void) {
  reset_fixture();
  CHECK(h2_passport_recording_prepare() == 0);
  CHECK(fixture.image_calls == 1 && fixture.get_calls == 1);
  CHECK(fixture.entropy.calls == 0);
  check_no_flash_changes();
  /* Make an unwanted second verifier invocation fail. */
  fixture.verify_result = ESP_FAIL;
  CHECK(store->open(store->user, 1, FRAME_COUNT, 256) == 0);
  CHECK(fixture.image_calls == 1 && fixture.get_calls == 2);
  CHECK(fixture.entropy.calls == 1);
  CHECK(store->begin(store->user, 0) == 0);
  write_pcm(0, 0, 256);
  check_read(0, 0, 256);
  CHECK(h2_passport_recording_prepare() == H2_PAL_ERR_INVALID_STATE);
  check_read(0, 0, 256);
  CHECK(store->close(store->user) == 0);
  CHECK(store->open(store->user, 1, FRAME_COUNT, 256) == H2_PAL_ERR_FORMAT);
  CHECK(fixture.image_calls == 2);
  CHECK(store->close(store->user) == 0);
  fixture.verify_result = ESP_OK;
  CHECK(store->open(store->user, 1, FRAME_COUNT, 256) == 0);
  CHECK(fixture.image_calls == 3);
  CHECK(store->close(store->user) == 0);
  check_outside_recordings(1, FRAME_COUNT, 256);
}

static void test_prepare_state_changes(void) {
  enum change {
    SAME_BIND, NEW_RUNTIME, NEW_LOADER, NEW_DISK_API, NEW_DISK_VTABLE,
    IMAGE_CHECKSUM, PACKAGE_CHECKSUM, IMAGE_SIZE, PACKAGE_SIZE, VERSION, BOARD,
    TARGET, VALID_VALUE, LOGICAL_RUNNING, WRITE_ALIGNMENT, DISK_FLAGS,
    APP_ADDRESS_CHANGED, RUNNING_ADDRESS, RUNNING_SIZE, CHANGE_COUNT,
  };
  for (unsigned change = 0; change < CHANGE_COUNT; ++change) {
    reset_fixture();
    CHECK(h2_passport_recording_prepare() == 0);
    CHECK(fixture.image_calls == 1);
    h2_runtime_t other_runtime = fixture.runtime;
    h2_loader_t other_loader = fixture.loader;
    h2_pal_disk_api_t other_disk = fixture.disk;
    h2_pal_disk_vtable_t other_vtable = disk_vtable;
    switch (change) {
      case SAME_BIND:
        h2_passport_recording_bind(&fixture.runtime, &fixture.loader);
        break;
      case NEW_RUNTIME:
        h2_passport_recording_bind(&other_runtime, &fixture.loader);
        break;
      case NEW_LOADER:
        h2_passport_recording_bind(&fixture.runtime, &other_loader);
        break;
      case NEW_DISK_API:
        fixture.runtime.disk = &other_disk;
        break;
      case NEW_DISK_VTABLE:
        fixture.disk.vtable = &other_vtable;
        break;
      case IMAGE_CHECKSUM:
        fixture.loader.status.partition_2.image_checksum[0] = 'a';
        break;
      case PACKAGE_CHECKSUM:
        fixture.loader.status.partition_2.package_checksum[0] = 'b';
        break;
      case IMAGE_SIZE:
        fixture.loader.status.partition_2.image_size += 16;
        break;
      case PACKAGE_SIZE:
        ++fixture.loader.status.partition_2.package_size;
        break;
      case VERSION:
        fixture.loader.status.partition_2.version[0] = '2';
        break;
      case BOARD:
        fixture.loader.status.partition_2.board[0] = 'x';
        break;
      case TARGET:
        fixture.loader.status.partition_2.target[0] = 'y';
        break;
      case VALID_VALUE:
        fixture.loader.status.partition_2.valid = 2;
        break;
      case LOGICAL_RUNNING:
        fixture.loader.config.h2loader_partition_id = 3;
        fixture.loader.status.running_partition_id = 3;
        break;
      case WRITE_ALIGNMENT:
        fixture.partition.write_alignment = 16;
        break;
      case DISK_FLAGS:
        fixture.partition.flags &= ~H2_PAL_DISK_PARTITION_FLAG_BOOTABLE;
        break;
      case APP_ADDRESS_CHANGED:
        fixture.app.address += BLOCK_SIZE;
        break;
      case RUNNING_ADDRESS:
        fixture.running.address += BLOCK_SIZE;
        break;
      case RUNNING_SIZE:
        --fixture.running.size;
        break;
      default:
        CHECK(0);
    }
    /* These changes still pass basic guards, so each must reach the verifier
     * rather than reuse validation of a different image/provider/partition. */
    fixture.verify_result = ESP_FAIL;
    CHECK(store->open(store->user, 1, FRAME_COUNT, 256) == H2_PAL_ERR_FORMAT);
    CHECK(fixture.image_calls == 2);
    CHECK(store->close(store->user) == 0);
    check_no_flash_changes();
    h2_passport_recording_bind(&fixture.runtime, &fixture.loader);
  }
}

static void test_prepare_token_invalidation(void) {
  enum invalidation {
    BAD_DIMENSIONS, NO_CAPACITY, CLOSED, PREPARE_VERIFY_FAILURE,
    PREPARE_GUARD_FAILURE, OPEN_GUARD_FAILURE, INVALIDATION_COUNT,
  };
  for (unsigned reason = 0; reason < INVALIDATION_COUNT; ++reason) {
    reset_fixture();
    CHECK(h2_passport_recording_prepare() == 0);
    fixture.verify_result = ESP_FAIL;
    unsigned expected_verifications = 2;
    switch (reason) {
      case BAD_DIMENSIONS:
        CHECK(store->open(store->user, 0, FRAME_COUNT, 256) ==
              H2_PAL_ERR_INVALID_ARG);
        break;
      case NO_CAPACITY:
        CHECK(store->open(store->user, 2, UINT_MAX, 256) == H2_PAL_ERR_NO_SPACE);
        break;
      case CLOSED:
        CHECK(store->close(store->user) == 0);
        break;
      case PREPARE_VERIFY_FAILURE:
        CHECK(h2_passport_recording_prepare() == H2_PAL_ERR_FORMAT);
        ++expected_verifications;
        break;
      case PREPARE_GUARD_FAILURE:
        fixture.loader.status.stage.valid = 1;
        CHECK(h2_passport_recording_prepare() == H2_PAL_ERR_INVALID_STATE);
        fixture.loader.status.stage.valid = 0;
        break;
      case OPEN_GUARD_FAILURE:
        fixture.loader.status.stage.valid = 1;
        CHECK(store->open(store->user, 1, FRAME_COUNT, 256) ==
              H2_PAL_ERR_INVALID_STATE);
        fixture.loader.status.stage.valid = 0;
        break;
      default:
        CHECK(0);
    }
    CHECK(store->open(store->user, 1, FRAME_COUNT, 256) == H2_PAL_ERR_FORMAT);
    CHECK(fixture.image_calls == expected_verifications);
    CHECK(store->close(store->user) == 0);
    CHECK(fixture.entropy.calls == 0);
    check_no_flash_changes();
  }
}

static void test_frame_contract(void) {
  reset_fixture();
  int16_t pcm[MAX_SAMPLES] = {0};
  CHECK(store->begin(store->user, 0) != 0);
  CHECK(store->write_frame(store->user, 0, 0, pcm, 256) != 0);
  CHECK(store->read_frame(store->user, 0, 0, pcm, 256) != 0);
  CHECK(store->open(store->user, 1, 2, 256) == 0);
  CHECK(store->open(store->user, 1, 2, 256) == H2_PAL_ERR_INVALID_STATE);
  CHECK(store->write_frame(store->user, 0, 0, pcm, 256) != 0);
  CHECK(store->read_frame(store->user, 0, 0, pcm, 256) != 0);
  CHECK(store->begin(store->user, 1) != 0);
  CHECK(store->begin(store->user, 0) == 0);
  CHECK(store->write_frame(store->user, 0, 1, pcm, 256) != 0);
  CHECK(store->write_frame(store->user, 0, 0, NULL, 256) != 0);
  CHECK(store->write_frame(store->user, 0, 0, pcm, 320) != 0);
  CHECK(store->write_frame(store->user, 1, 0, pcm, 256) != 0);
  CHECK(store->write_frame(store->user, 0, UINT_MAX, pcm, 256) != 0);
  CHECK(fixture.write_calls == 0);
  write_pcm(0, 0, 256);
  CHECK(store->write_frame(store->user, 0, 0, pcm, 256) != 0);
  CHECK(store->read_frame(store->user, 0, 1, pcm, 256) != 0);
  CHECK(store->read_frame(store->user, 0, 0, NULL, 256) != 0);
  CHECK(store->read_frame(store->user, 0, 0, pcm, 320) != 0);
  CHECK(store->read_frame(store->user, 1, 0, pcm, 256) != 0);
  CHECK(store->read_frame(store->user, 0, UINT_MAX, pcm, 256) != 0);
  CHECK(fixture.read_calls == 0 && fixture.write_calls == 1);
  check_read(0, 0, 256);
  write_pcm(0, 1, 256);
  check_read(0, 1, 256);
  CHECK(store->write_frame(store->user, 0, 2, pcm, 256) != 0);
  CHECK(store->close(store->user) == 0);
  check_outside_recordings(1, 2, 256);
}

int main(void) {
  test_name = "wolfCrypt initialization";
  h2_wolfcrypt_crypto_config_t crypto = {
      .entropy_user = &fixture.entropy, .entropy = entropy_fill};
  CHECK(h2_wolfcrypt_crypto_init(&crypto) == 0);
#define RUN(test)            \
  do {                       \
    test_name = #test;        \
    test();                  \
    printf("PASS %s\n", #test); \
  } while (0)
  RUN(test_open_guards);
  RUN(test_invalid_dimensions);
  RUN(test_full_recordings);
  RUN(test_retry_renews_nonce);
  RUN(test_tamper_and_read_failure);
  RUN(test_partial_write_requires_begin);
  RUN(test_begin_failures_disable_recording);
  RUN(test_failed_close_retains_ownership_and_clears_key);
  RUN(test_failed_open_cleanup);
  RUN(test_prepare_verifies_once);
  RUN(test_prepare_state_changes);
  RUN(test_prepare_token_invalidation);
  RUN(test_frame_contract);
#undef RUN
  h2_wolfcrypt_crypto_deinit();
  return 0;
}
