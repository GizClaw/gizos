#include "asm/cpu.h"
#include "h2_jieli_wl82_platform_image.h"
#include "h2_atomic.h"
#include "jieli_warm_boot.h"
#include "jieli_native_image.h"
#include "jieli_upgrade_io.h"
#include "asm/includes.h"
#include "os/os_api.h"
#include "timer.h"
#include "update/dual_bank_updata_api.h"
#include <string.h>

extern int snprintf(char* buffer, size_t size, const char* format, ...);
extern uint32_t get_target_udate_addr(void);
extern uint32_t decode_data_by_user_key(uint16_t key, uint8_t* data,
                                        uint16_t size, uint32_t address,
                                        uint8_t block);
#define H2_JIELI_UPDATE_BLOCK_SIZE 4096u
#define H2_JIELI_UPDATE_WAIT_TICKS 500u
#define BOOT_EVIDENCE_V1 1u
#define BOOT_EVIDENCE_SIZE 40u

/* Optional diagnostic sink; no application object is borrowed by callbacks. */
__attribute__((weak)) void h2_jieli_wl82_diag_write(const char* text) {
  (void)text;
}

typedef struct h2_jieli_image_state {
  h2_jieli_wl82_image_config_t config;
  const h2_pal_fs_api_t* fs;
  int configured;
  int poisoned;
  int callbacks_initialized;
  uint32_t running_partition_id;
  uint32_t next_partition_id;
  uint32_t writer_partition_id;
  uint32_t execution_partition_id;
  h2_pal_fs_file_t* shadow;
  h2_pal_fs_file_t* reader;
  uint32_t reader_partition_id;
  uint64_t reader_size;
  /* Boot-lifetime storage: never delete/reinitialize under late callbacks. */
  OS_SEM update_sem;
  h2_atomic_bool_t burn_waiting;
  uint8_t update_buffer[H2_JIELI_UPDATE_BLOCK_SIZE];
  size_t update_buffered;
  uint64_t update_native_written;
  h2_jieli_native_image_t native_image;
  uint64_t expected;
  uint64_t written;
  uint64_t committed_size;
  uint32_t committed_partition_id;
  int update_active;
  volatile int update_result;
  int update_committed;
  int prepared;
  h2_pal_power_boot_policy_t prepared_policy;
  int running_confirmed;
  int confirmed_evidence_valid;
  uint8_t confirmed_header[H2_JIELI_UPGRADE_HEADER_SIZE];
  uint32_t warm_partition_id;
  int warm_loader_return_allowed;
  uint16_t reboot_timer_id;
} h2_jieli_image_state_t;
static h2_jieli_image_state_t state;

#define BANK_1_ID (state.config.bank_1_partition_id)
#define BANK_2_ID (state.config.bank_2_partition_id)
#define BANK_1_BASE (h2_jieli_wl82_boot_geometry()->bank_1_sfc_base)
#define BANK_2_BASE (h2_jieli_wl82_boot_geometry()->bank_2_sfc_base)

static int clear_update_boot_info(void) {
  state.warm_loader_return_allowed = 0;
  return flash_update_clr_boot_info(CLEAR_APP_UPDATE_BANK);
}

static void image_reader_close(void) {
  if (state.reader != NULL) {
    (void)h2_pal_fs_close(state.fs, state.reader);
    state.reader = NULL;
  }
  state.reader_partition_id = 0u;
  state.reader_size = 0u;
}

static int image_path(uint32_t partition_id, char* out_path,
                      size_t out_path_size) {
  const char* path = partition_id == BANK_1_ID ? state.config.bank_1_shadow_path
                     : partition_id == BANK_2_ID
                         ? state.config.bank_2_shadow_path
                         : NULL;
  if (path == NULL) return H2_PAL_ERR_NOT_FOUND;
  size_t len = strlen(path);
  if (len >= out_path_size) return H2_PAL_ERR_NO_SPACE;
  memcpy(out_path, path, len + 1u);
  return H2_PAL_OK;
}

static void writer_abort_internal(void) {
  image_reader_close();
  if (state.update_active) {
    if (dual_bank_passive_update_exit(NULL) != 0u)
      state.poisoned = 1;
    else
      state.update_active = 0;
  }
  if (state.shadow != NULL) {
    if (h2_pal_fs_close(state.fs, state.shadow) != H2_PAL_OK)
      state.poisoned = 1;
    state.shadow = NULL;
  }
  if (state.fs != NULL) {
    (void)h2_pal_fs_remove(state.fs, state.config.shadow_temp_path);
    if (state.committed_partition_id != 0u && !state.update_committed) {
      char path[160];
      if (image_path(state.committed_partition_id, path, sizeof(path)) ==
          H2_PAL_OK) {
        (void)h2_pal_fs_remove(state.fs, path);
      }
    }
  }
  state.writer_partition_id = 0u;
  state.update_buffered = 0u;
  state.update_native_written = 0u;
  state.expected = 0u;
  state.written = 0u;
  state.committed_partition_id = 0u;
  state.committed_size = 0u;
  state.update_committed = 0;
  state.prepared = 0;
  state.execution_partition_id = 0u;
}

static int image_get_capacity(void* user, uint32_t partition_id,
                              uint64_t* out_capacity) {
  (void)user;
  if (out_capacity == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (partition_id == BANK_1_ID || partition_id == BANK_2_ID) {
    /* These are logical A/B roles. JieLi's packer owns the physical bank
     * placement; CODE_BOUNDARY_LINE is the maximum image size of either
     * executable bank. */
    *out_capacity = state.config.image_capacity;
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static int image_read(void* user, uint32_t partition_id,
                      h2_pal_image_view_t view, uint64_t offset, void* data,
                      size_t len) {
  char line[144];
  char path[160];
  h2_pal_fs_stat_t stat_value;
  size_t read = 0u;
  (void)user;
  if (view == H2_PAL_IMAGE_VIEW_NATIVE) return H2_PAL_ERR_UNSUPPORTED;
  if (!state.configured) return H2_PAL_ERR_UNAVAILABLE;
  if ((data == NULL && len != 0u) || state.fs == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  /* JieLi's passive updater transforms the UFW byte stream before storing it
   * in the inactive flash bank, so a physical-bank SHA is not comparable to
   * the manifest SHA. The persistent SD shadow preserves the exact input
   * bytes across the trial and canonical reboots and is the canonical reader
   * for common Loader verification/copy. A stale shadow is harmless because
   * the common layer verifies its size and SHA against the persisted upgrade
   * record before selecting either bank. */
  int rc = image_path(partition_id, path, sizeof(path));
  const char* step = "path";
  if (rc == H2_PAL_OK &&
      (state.reader == NULL || state.reader_partition_id != partition_id)) {
    image_reader_close();
    memset(&stat_value, 0, sizeof(stat_value));
    if (state.committed_partition_id == partition_id &&
        state.committed_size != 0u) {
      stat_value.size = state.committed_size;
    } else {
      step = "stat";
      rc = h2_pal_fs_stat(state.fs, path, &stat_value);
    }
    if (rc == H2_PAL_OK && stat_value.is_dir) {
      step = "stat-directory";
      rc = H2_PAL_ERR_NOT_FOUND;
    }
    if (rc == H2_PAL_OK) {
      step = "open";
      rc = h2_pal_fs_open(state.fs, path, H2_PAL_FS_OPEN_READ, &state.reader);
    }
    if (rc == H2_PAL_OK) {
      state.reader_partition_id = partition_id;
      state.reader_size = stat_value.size;
    }
  }
  if (rc == H2_PAL_OK &&
      (offset > state.reader_size || len > state.reader_size - offset)) {
    step = "bounds";
    rc = H2_PAL_ERR_NOT_FOUND;
  }
  if (rc == H2_PAL_OK) {
    step = "seek";
    rc = h2_pal_fs_seek(state.fs, state.reader, offset);
  }
  if (rc == H2_PAL_OK) {
    step = "read";
    rc = h2_pal_fs_read(state.fs, state.reader, data, len, &read);
  }
  if (rc == H2_PAL_OK && read != len) rc = H2_PAL_ERR_TRUNCATED;
  if (rc == H2_PAL_OK && (offset == 0u || offset + len == state.reader_size)) {
    (void)snprintf(
        line, sizeof(line),
        "H2_JIELI_IMAGE_READ_SHADOW partition=%u offset=%u bytes=%u\r\n",
        (unsigned)partition_id, (unsigned)offset, (unsigned)len);
    h2_jieli_wl82_diag_write(line);
  }
  if (rc != H2_PAL_OK) {
    (void)snprintf(
        line, sizeof(line),
        "H2_JIELI_IMAGE_READ_SD_ERROR step=%s partition=%u offset=%u bytes=%u read=%u rc=%d\r\n",
        step, (unsigned)partition_id, (unsigned)offset, (unsigned)len,
        (unsigned)read, rc);
    h2_jieli_wl82_diag_write(line);
  }
  if (rc != H2_PAL_OK || offset + len == state.reader_size) {
    image_reader_close();
  }
  return rc;
}

static int image_writer_begin(void* user,
                              const h2_pal_image_write_request_t* request) {
  char line[128];
  uint64_t capacity = 0u;
  (void)user;
  if (!state.configured) return H2_PAL_ERR_UNAVAILABLE;
  if (request == NULL || request->image_size == 0u ||
      request->image_size > UINT32_MAX ||
      request->partition_id == state.running_partition_id ||
      (request->execution_partition_id != 0u &&
       request->execution_partition_id != BANK_1_ID &&
       request->execution_partition_id != BANK_2_ID))
    return H2_PAL_ERR_INVALID_ARG;
  if (state.poisoned || h2_jieli_upgrade_erase_failed()) return H2_PAL_ERR_IO;
  if (state.update_active || state.writer_partition_id != 0u ||
      (state.committed_partition_id != 0u && !state.update_committed) ||
      state.reboot_timer_id != 0u)
    return H2_PAL_ERR_BUSY;
  /* An update may change the ROM choice; stop relying on the boot receipt. */
  state.warm_loader_return_allowed = 0;
  /* Beginning canonical copy must not invoke public abort: the activated
   * second-bank header is the recovery barrier while bank one is erased. */
  writer_abort_internal();
  if (state.poisoned) return H2_PAL_ERR_IO;
  const uint32_t partition_id = request->partition_id;
  int rc = image_get_capacity(NULL, partition_id, &capacity);
  if (rc != H2_PAL_OK || request->image_size > capacity) {
    return rc == H2_PAL_OK ? H2_PAL_ERR_NO_SPACE : rc;
  }
  rc = h2_pal_fs_open(state.fs, state.config.shadow_temp_path,
                      H2_PAL_FS_OPEN_WRITE_TRUNCATE, &state.shadow);
  if (rc != H2_PAL_OK) return rc;
  /* Discard a completion posted after an earlier burn wait gave up. */
  if (os_sem_set(&state.update_sem, 0) != OS_NO_ERR) {
    writer_abort_internal();
    return H2_PAL_ERR_IO;
  }
  state.update_active = 1;
  uint32_t image_size = (uint32_t)request->image_size;
  /* This writer consumes JieLi's self-describing UFW stream.  Match
   * net_update.c and pass zero CRC/size so the native updater parses the UFW
   * header.  Non-zero CRC/size selects the raw-firmware mode used by TWS. */
  h2_jieli_wl82_diag_write("H2_JIELI_UPDATE_INIT_ENTER\r\n");
  uint32_t init_rc =
      dual_bank_passive_update_init(0u, 0u, H2_JIELI_UPDATE_BLOCK_SIZE, NULL);
  (void)snprintf(line, sizeof(line), "H2_JIELI_UPDATE_INIT_RETURN rc=%u\r\n",
                 (unsigned)init_rc);
  h2_jieli_wl82_diag_write(line);
  uint32_t allow_rc = init_rc == 0u ? dual_bank_update_allow_check(0u) : 0u;
  (void)snprintf(line, sizeof(line),
                 "H2_JIELI_UPDATE_BEGIN bytes=%u init=%u allow=%u\r\n",
                 (unsigned)image_size, (unsigned)init_rc, (unsigned)allow_rc);
  h2_jieli_wl82_diag_write(line);
  if (init_rc != 0u || allow_rc != 0u) {
    writer_abort_internal();
    (void)clear_update_boot_info();
    return H2_PAL_ERR_NO_SPACE;
  }
  state.writer_partition_id = partition_id;
  state.execution_partition_id = request->execution_partition_id;
  state.update_buffered = 0u;
  state.update_native_written = 0u;
  state.update_result = H2_PAL_OK;
  state.expected = request->image_size;
  state.written = 0u;
  state.update_committed = 0;
  return H2_PAL_OK;
}

static int update_write_error(void* result) {
  /* Match JieLi's net_update implementation: this callback reports only an
   * asynchronous programming error.  A successful write does not call it. */
  if (result != NULL) state.update_result = H2_PAL_ERR_IO;
  return 0;
}

static int update_write_block(const uint8_t* data, size_t len,
                              int report_async_errors) {
  char line[112];
  if (len == 0u || len > UINT16_MAX) return H2_PAL_ERR_INVALID_ARG;
  if (state.update_result != H2_PAL_OK) return state.update_result;
  const int trace = state.update_native_written >= 512u * 1024u;
  if (trace) {
    (void)snprintf(line, sizeof(line),
                   "H2_JIELI_UPDATE_BLOCK_ENTER offset=%u bytes=%u\r\n",
                   (unsigned)state.update_native_written, (unsigned)len);
    h2_jieli_wl82_diag_write(line);
  }
  uint32_t native_rc =
      dual_bank_update_write((void*)data, (uint16_t)len,
                             report_async_errors ? update_write_error : NULL);
  if (trace) {
    (void)snprintf(line, sizeof(line),
                   "H2_JIELI_UPDATE_BLOCK_RETURN offset=%u bytes=%u rc=%u\r\n",
                   (unsigned)state.update_native_written, (unsigned)len,
                   (unsigned)native_rc);
    h2_jieli_wl82_diag_write(line);
  }
  if (native_rc != 0u || h2_jieli_upgrade_erase_failed()) {
    (void)snprintf(
        line, sizeof(line),
        "H2_JIELI_UPDATE_WRITE_ERROR offset=%u bytes=%u native=%u\r\n",
        (unsigned)state.update_native_written, (unsigned)len,
        (unsigned)native_rc);
    h2_jieli_wl82_diag_write(line);
    return H2_PAL_ERR_IO;
  }
  return state.update_result;
}

static int update_flush_buffer(void) {
  size_t buffered = state.update_buffered;
  if (state.update_native_written == 0u) {
    if (h2_jieli_native_image_parse(state.update_buffer, buffered,
                                    (uint32_t)state.expected,
                                    &state.native_image) != 0) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    char line[128];
    (void)snprintf(line, sizeof(line),
                   "H2_JIELI_NATIVE_IMAGE offset=%u bytes=%u crc=%04x\r\n",
                   (unsigned)state.native_image.code_offset,
                   (unsigned)state.native_image.code_length,
                   (unsigned)state.native_image.code_crc);
    h2_jieli_wl82_diag_write(line);
    uint32_t target = get_target_udate_addr();
    uint32_t aligned = (target + H2_JIELI_UPDATE_BLOCK_SIZE - 1u) &
                       ~(H2_JIELI_UPDATE_BLOCK_SIZE - 1u);
    size_t first = aligned - target;
    if (first > buffered) first = buffered;
    if (first != 0u) {
      /* Match JieLi's net_update implementation: the initial fragment that
       * reaches the first 4 KiB boundary is queued without a callback.  Keep
       * the tail buffered until later input makes the next complete packet;
       * submitting the short tail by itself corrupts the upgraded image. */
      int rc = update_write_block(state.update_buffer, first, 0);
      if (rc != H2_PAL_OK) return rc;
      state.update_native_written += first;
      state.update_buffered = buffered - first;
      if (state.update_buffered != 0u) {
        memmove(state.update_buffer, state.update_buffer + first,
                state.update_buffered);
      }
      return H2_PAL_OK;
    }
  }
  if (buffered != 0u) {
    int rc = update_write_block(state.update_buffer, buffered, 1);
    if (rc != H2_PAL_OK) return rc;
  }
  state.update_native_written += buffered;
  state.update_buffered = 0u;
  return H2_PAL_OK;
}

/* Diagnostic fixtures may hold a verified physical flash boundary for a
 * manual power cut. Production always keeps the normal bounded wait. */
__attribute__((weak)) int h2_jieli_loader_powercut_paused(void) { return 0; }

static int update_burn_complete(int error) {
  char line[96];
  /* After a timed-out wait the result belongs to nobody; only the post of
   * the process-lifetime semaphore remains, and the next begin drains it. */
  if (h2_atomic_bool_load(&state.burn_waiting, H2_ATOMIC_ACQUIRE)) {
    state.update_result = error == 0 && !h2_jieli_upgrade_erase_failed()
                              ? H2_PAL_OK
                              : H2_PAL_ERR_IO;
  }
  (void)snprintf(line, sizeof(line),
                 "H2_JIELI_UPDATE_BURN_CALLBACK error=%d\r\n", error);
  h2_jieli_wl82_diag_write(line);
  os_sem_post(&state.update_sem);
  return 0;
}

static int image_writer_write(void* user, const void* data, size_t len) {
  size_t shadow_written = 0u;
  (void)user;
  if (!state.update_active || state.shadow == NULL ||
      (data == NULL && len != 0u) || state.written > state.expected ||
      len > state.expected - state.written) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (len == 0u) return H2_PAL_OK;
  int rc = h2_pal_fs_write(state.fs, state.shadow, data, len, &shadow_written);
  if (rc != H2_PAL_OK || shadow_written != len) {
    writer_abort_internal();
    return rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc;
  }
  const uint8_t* cursor = data;
  size_t remaining = len;
  while (remaining != 0u) {
    size_t take = H2_JIELI_UPDATE_BLOCK_SIZE - state.update_buffered;
    if (take > remaining) take = remaining;
    memcpy(state.update_buffer + state.update_buffered, cursor, take);
    state.update_buffered += take;
    cursor += take;
    remaining -= take;
    if (state.update_buffered == H2_JIELI_UPDATE_BLOCK_SIZE) {
      rc = update_flush_buffer();
      if (rc != H2_PAL_OK) {
        writer_abort_internal();
        (void)clear_update_boot_info();
        return rc;
      }
    }
  }
  state.written += len;
  return H2_PAL_OK;
}

static int image_writer_finish(void* user) {
  char line[128];
  char final_path[160];
  uint32_t partition_id = state.writer_partition_id;
  (void)user;
  h2_jieli_wl82_diag_write("H2_JIELI_UPDATE_FINISH_ENTER\r\n");
  if (!state.update_active || state.shadow == NULL ||
      state.written != state.expected) {
    writer_abort_internal();
    return H2_PAL_ERR_INVALID_STATE;
  }
  int rc = update_flush_buffer();
  /* If the first native fragment reached its 4 KiB boundary only here, the
   * remaining short tail has never formed a subsequent SDK packet. Publishing
   * the complete shadow would hide those missing native bytes from STORED SHA
   * verification. Reject before rename; never fabricate/pad an SDK packet. */
  if (rc == H2_PAL_OK && (state.update_buffered != 0u ||
                          state.update_native_written != state.expected))
    rc = H2_PAL_ERR_TRUNCATED;
  if (rc == H2_PAL_OK && state.execution_partition_id != 0u &&
      state.execution_partition_id != BANK_1_ID &&
      state.execution_partition_id != BANK_2_ID)
    rc = H2_PAL_ERR_FORMAT;
  if (h2_jieli_upgrade_erase_failed()) rc = H2_PAL_ERR_IO;
  if (rc == H2_PAL_OK && state.update_result != H2_PAL_OK) {
    rc = state.update_result;
  }
  (void)snprintf(
      line, sizeof(line),
      "H2_JIELI_UPDATE_WRITE_DONE expected=%u native=%u result=%d\r\n",
      (unsigned)state.expected, (unsigned)state.update_native_written,
      state.update_result);
  h2_jieli_wl82_diag_write(line);
  if (rc == H2_PAL_OK) rc = h2_pal_fs_sync(state.fs, state.shadow);
  int close_rc = h2_pal_fs_close(state.fs, state.shadow);
  state.shadow = NULL;
  if (rc == H2_PAL_OK) rc = close_rc;
  if (rc == H2_PAL_OK)
    rc = image_path(partition_id, final_path, sizeof(final_path));
  if (rc == H2_PAL_OK) {
    (void)h2_pal_fs_remove(state.fs, final_path);
    rc = h2_pal_fs_rename(state.fs, state.config.shadow_temp_path, final_path);
  }
  if (rc != H2_PAL_OK) {
    (void)clear_update_boot_info();
    writer_abort_internal();
    return rc;
  }
  /* Keep the native update session open while the common Loader hashes the
   * destination bank. Boot Info is committed later by power_set_next(), only
   * after package verification and preference updates have succeeded. */
  state.committed_partition_id = partition_id;
  state.committed_size = state.expected;
  state.prepared = 0;
  state.writer_partition_id = 0u;
  state.expected = 0u;
  state.written = 0u;
  return H2_PAL_OK;
}

static int image_writer_abort(void* user) {
  (void)user;
  if (!state.configured) return H2_PAL_ERR_UNAVAILABLE;
  writer_abort_internal();
  int cleared = clear_update_boot_info();
  return !state.poisoned && cleared == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int image_get_info(void* user, uint32_t id, h2_pal_image_view_t view,
                          h2_pal_image_info_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!state.configured) return H2_PAL_ERR_UNAVAILABLE;
  if (view == H2_PAL_IMAGE_VIEW_NATIVE) {
    if (id != state.running_partition_id) return H2_PAL_ERR_UNSUPPORTED;
    struct BootInfo native;
    memset(&native, 0, sizeof(native));
    if (get_current_boot_info(&native) != 0 || native.codeLength == 0u ||
        native.codeLength > state.config.image_capacity ||
        native.baseAddress != (id == BANK_1_ID ? BANK_1_BASE : BANK_2_BASE))
      return H2_PAL_ERR_UNAVAILABLE;
    out->capacity = state.config.image_capacity;
    out->image_size = native.codeLength;
    out->flags = H2_PAL_IMAGE_FLAG_SIZE_KNOWN;
    return H2_PAL_OK;
  }
  int rc = image_get_capacity(NULL, id, &out->capacity);
  if (rc != H2_PAL_OK) return rc;
  out->flags =
      id != state.running_partition_id ? H2_PAL_IMAGE_FLAG_WRITABLE : 0u;
  if (state.committed_partition_id == id && state.committed_size != 0u) {
    out->image_size = state.committed_size;
    out->flags |= H2_PAL_IMAGE_FLAG_READABLE | H2_PAL_IMAGE_FLAG_SIZE_KNOWN;
    if (!state.update_committed) out->flags |= H2_PAL_IMAGE_FLAG_PREPARED;
    return H2_PAL_OK;
  }
  char path[160];
  h2_pal_fs_stat_t stat_value = {0};
  rc = image_path(id, path, sizeof(path));
  if (rc == H2_PAL_OK) rc = h2_pal_fs_stat(state.fs, path, &stat_value);
  if (rc == H2_PAL_ERR_NOT_FOUND) return H2_PAL_OK;
  if (rc != H2_PAL_OK) return rc;
  if (!stat_value.is_dir && stat_value.size != 0u &&
      stat_value.size <= state.config.image_capacity) {
    out->image_size = stat_value.size;
    out->flags |= H2_PAL_IMAGE_FLAG_READABLE | H2_PAL_IMAGE_FLAG_SIZE_KNOWN;
  }
  return H2_PAL_OK;
}

static int native_header_valid(const h2_pal_power_native_evidence_t* evidence) {
  if (evidence == NULL || evidence->format != BOOT_EVIDENCE_V1 ||
      evidence->size != BOOT_EVIDENCE_SIZE || evidence->bytes[6] != 0u ||
      evidence->bytes[7] != 0u)
    return 0;
  uint32_t length = h2_jieli_native_u32(evidence->bytes);
  if (length == 0u || length > state.config.image_capacity) return 0;
  uint8_t plain[H2_JIELI_UPGRADE_HEADER_SIZE];
  memcpy(plain, evidence->bytes + 8u, sizeof(plain));
  (void)decode_data_by_user_key(0xffffu, plain, sizeof(plain), 0u, 32u);
  return h2_jieli_native_u16(plain) == h2_jieli_native_header_crc(plain) &&
         h2_jieli_native_u16(plain + 4u) ==
             h2_jieli_native_u16(evidence->bytes + 4u) &&
         h2_jieli_native_u32(plain + 8u) == length;
}

static int native_burn(int defer_header) {
  char line[128];
  int rc = H2_PAL_OK;
  state.update_result = H2_PAL_ERR_TIMEOUT;
  if (defer_header && h2_jieli_upgrade_header_arm() != 0) return H2_PAL_ERR_IO;
  h2_atomic_bool_store(&state.burn_waiting, true, H2_ATOMIC_RELEASE);
  uint32_t burn_rc = dual_bank_update_burn_boot_info(update_burn_complete);
  int pend_rc = OS_NO_ERR;
  if (burn_rc != 0u)
    rc = H2_PAL_ERR_IO;
  else if ((pend_rc = os_sem_pend(&state.update_sem,
                                  H2_JIELI_UPDATE_WAIT_TICKS)) != OS_NO_ERR) {
    while (h2_jieli_loader_powercut_paused()) os_time_dly(100u);
    rc = H2_PAL_ERR_TIMEOUT;
  }
  h2_atomic_bool_store(&state.burn_waiting, false, H2_ATOMIC_RELEASE);
  if (rc == H2_PAL_OK) rc = state.update_result;
  if (h2_jieli_upgrade_erase_failed()) rc = H2_PAL_ERR_IO;
  (void)snprintf(line, sizeof(line),
                 "H2_JIELI_UPDATE_BURN call=%u pend=%d result=%d\r\n",
                 (unsigned)burn_rc, pend_rc, rc);
  h2_jieli_wl82_diag_write(line);
  return rc;
}

static int native_exit(void) {
  if (!state.update_active) return H2_PAL_OK;
  if (dual_bank_passive_update_exit(NULL) != 0u) {
    state.poisoned = 1;
    return H2_PAL_ERR_IO;
  }
  state.update_active = 0;
  return h2_jieli_upgrade_erase_failed() ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static int power_prepare(void* user, const h2_pal_power_boot_request_t* request,
                         h2_pal_power_native_evidence_t* out_evidence) {
  (void)user;
  if (request == NULL || out_evidence == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_evidence, 0, sizeof(*out_evidence));
  if (!state.configured) return H2_PAL_ERR_UNAVAILABLE;
  if (request->partition_id != BANK_1_ID && request->partition_id != BANK_2_ID)
    return H2_PAL_ERR_NOT_FOUND;
  if (state.reboot_timer_id != 0u || h2_jieli_upgrade_erase_failed())
    return H2_PAL_ERR_INVALID_STATE;
  if (request->policy != H2_PAL_POWER_BOOT_POLICY_EXISTING &&
      request->policy != H2_PAL_POWER_BOOT_POLICY_TRIAL &&
      request->policy != H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY)
    return H2_PAL_ERR_INVALID_ARG;
  if (request->policy == H2_PAL_POWER_BOOT_POLICY_EXISTING &&
      request->partition_id == state.running_partition_id) {
    /* Read-only current native observation, including while another candidate
     * is open. No next-partition, session, header, or activation state changes.
     */
    struct BootInfo native;
    _Static_assert(sizeof(native) <= H2_PAL_POWER_NATIVE_EVIDENCE_MAX,
                   "native BootInfo exceeds evidence storage");
    memset(&native, 0, sizeof(native));
    if (get_current_boot_info(&native) != 0 || native.codeLength == 0u ||
        native.baseAddress != (state.running_partition_id == BANK_1_ID
                                   ? BANK_1_BASE
                                   : BANK_2_BASE))
      return H2_PAL_ERR_UNAVAILABLE;
    out_evidence->format = 2u;
    out_evidence->size = sizeof(native);
    memcpy(out_evidence->bytes, &native, sizeof(native));
    return H2_PAL_OK;
  }
  if (state.committed_partition_id != request->partition_id) {
    if (state.update_active ||
        request->policy != H2_PAL_POWER_BOOT_POLICY_EXISTING)
      return H2_PAL_ERR_INVALID_STATE;
    return H2_PAL_OK;
  }
  if (!state.update_active || state.writer_partition_id != 0u ||
      state.update_committed || state.prepared)
    return H2_PAL_ERR_INVALID_STATE;
  int rc = H2_PAL_OK;
  if (request->partition_id == BANK_2_ID) {
    if (request->policy == H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY) {
      rc = native_burn(1);
      if (rc == H2_PAL_OK) {
        out_evidence->format = BOOT_EVIDENCE_V1;
        out_evidence->size = BOOT_EVIDENCE_SIZE;
        for (unsigned i = 0u; i < 4u; ++i)
          out_evidence->bytes[i] =
              (uint8_t)(state.native_image.code_length >> (8u * i));
        out_evidence->bytes[4] = (uint8_t)state.native_image.code_crc;
        out_evidence->bytes[5] = (uint8_t)(state.native_image.code_crc >> 8u);
        if (h2_jieli_upgrade_header_copy(out_evidence->bytes + 8u) != 0 ||
            !native_header_valid(out_evidence))
          rc = H2_PAL_ERR_FORMAT;
      }
    }
    /* A trial never publishes BootInfo. Canonical-copy publication occurs only
     * at selection, after this preparation and the caller's metadata commit. */
    int exit_rc = native_exit();
    if (rc == H2_PAL_OK) rc = exit_rc;
  }
  if (rc != H2_PAL_OK) {
    memset(out_evidence, 0, sizeof(*out_evidence));
    (void)image_writer_abort(NULL);
    return rc;
  }
  state.prepared = 1;
  state.prepared_policy = request->policy;
  return H2_PAL_OK;
}

static void fill_partition(h2_pal_power_boot_partition_t* partition,
                           uint32_t id, uint32_t flags) {
  memset(partition, 0, sizeof(*partition));
  partition->id = id;
  partition->flags =
      H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE | flags |
      (id == BANK_1_ID ? H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY
                       : H2_PAL_POWER_BOOT_PARTITION_FLAG_APP);
  if (id == state.running_partition_id)
    partition->flags |= H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING;
  if (id == state.next_partition_id)
    partition->flags |= H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT;
  (void)snprintf(partition->name, sizeof(partition->name), "bank%u",
                 id == BANK_1_ID ? 1u : 2u);
}

static int power_get_capabilities(
    void* user, h2_pal_power_capabilities_t* out_capabilities) {
  (void)user;
  if (out_capabilities == NULL) return H2_PAL_ERR_INVALID_ARG;
  out_capabilities->flags = H2_PAL_POWER_CAPABILITY_REBOOT |
                            H2_PAL_POWER_CAPABILITY_BOOT_PARTITIONS |
                            H2_PAL_POWER_CAPABILITY_SET_NEXT_BOOT_PARTITION |
                            H2_PAL_POWER_CAPABILITY_IMAGE_STATE |
                            H2_PAL_POWER_CAPABILITY_PREPARE_BOOT |
                            H2_PAL_POWER_CAPABILITY_CONFIRM_IMAGE;
  return H2_PAL_OK;
}

static int power_list(void* user, h2_pal_power_boot_partition_cb_t callback,
                      void* callback_user) {
  (void)user;
  h2_pal_power_boot_partition_t partition;
  if (callback == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!state.configured) return H2_PAL_ERR_UNAVAILABLE;
  fill_partition(&partition, BANK_1_ID,
                 H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY);
  int rc = callback(callback_user, &partition);
  if (rc != H2_PAL_OK) return rc;
  fill_partition(&partition, BANK_2_ID, H2_PAL_POWER_BOOT_PARTITION_FLAG_APP);
  return callback(callback_user, &partition);
}

static int power_get_running(void* user,
                             h2_pal_power_boot_partition_t* out_partition) {
  (void)user;
  if (out_partition == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!state.configured || state.running_partition_id == 0u)
    return H2_PAL_ERR_INVALID_STATE;
  fill_partition(out_partition, state.running_partition_id,
                 H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING);
  return H2_PAL_OK;
}

static int power_get_next(void* user,
                          h2_pal_power_boot_partition_t* out_partition) {
  (void)user;
  if (out_partition == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!state.configured || state.next_partition_id == 0u)
    return H2_PAL_ERR_INVALID_STATE;
  fill_partition(out_partition, state.next_partition_id,
                 H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT);
  return H2_PAL_OK;
}

static int power_get_image_state(void* user, uint32_t id,
                                 h2_pal_power_image_state_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!state.configured) return H2_PAL_ERR_UNAVAILABLE;
  if (id != BANK_1_ID && id != BANK_2_ID) return H2_PAL_ERR_NOT_FOUND;
  memset(out, 0, sizeof(*out));
  out->state = H2_PAL_POWER_IMAGE_STATE_VALID;
  if (id == state.committed_partition_id && !state.update_committed)
    out->state = H2_PAL_POWER_IMAGE_STATE_NEW;
  else if (id == BANK_2_ID && id == state.running_partition_id &&
           !state.running_confirmed) {
    out->state = H2_PAL_POWER_IMAGE_STATE_PENDING_VERIFY;
    out->flags = H2_PAL_POWER_IMAGE_FLAG_CONFIRMATION_REQUIRED;
  }
  if (id == state.running_partition_id && state.confirmed_evidence_valid)
    out->flags |= H2_PAL_POWER_IMAGE_FLAG_RECOVERY_READY;
  return H2_PAL_OK;
}

static int power_confirm(void* user,
                         const h2_pal_power_native_evidence_t* evidence,
                         h2_pal_power_image_confirmation_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!state.configured || state.running_partition_id == 0u)
    return H2_PAL_ERR_INVALID_STATE;
  if (state.poisoned || h2_jieli_upgrade_erase_failed()) return H2_PAL_ERR_IO;
  struct BootInfo native;
  memset(&native, 0, sizeof(native));
  int native_rc = get_current_boot_info(&native);
  uint32_t mapped =
      h2_jieli_warm_boot_running_base(native_rc, native.baseAddress);
  if (mapped == 0u ||
      mapped !=
          (state.running_partition_id == BANK_1_ID ? BANK_1_BASE : BANK_2_BASE))
    return H2_PAL_ERR_IO;
  if (evidence != NULL && evidence->size != 0u) {
    if (mapped != BANK_2_BASE || !native_header_valid(evidence))
      return H2_PAL_ERR_FORMAT;
    if (native_rc == 0 && native.baseAddress == mapped &&
        native.codeLength != h2_jieli_native_u32(evidence->bytes))
      return H2_PAL_ERR_FORMAT;
    memcpy(state.confirmed_header, evidence->bytes + 8u,
           sizeof(state.confirmed_header));
    state.confirmed_evidence_valid = 1;
  }
  memset(out, 0, sizeof(*out));
  out->partition_id = state.running_partition_id;
  out->was_pending =
      state.running_partition_id == BANK_2_ID && !state.running_confirmed;
  if (state.confirmed_evidence_valid ||
      (native_rc == 0 && native.baseAddress == BANK_2_BASE &&
       native.codeLength != 0u))
    out->flags = H2_PAL_POWER_IMAGE_FLAG_RECOVERY_READY;
  state.running_confirmed = 1;
  return H2_PAL_OK;
}

static int publish_confirmed_bank_2(void) {
  if (!state.running_confirmed) return H2_PAL_ERR_INVALID_STATE;
  struct BootInfo native;
  memset(&native, 0, sizeof(native));
  if (get_current_boot_info(&native) == 0 &&
      native.baseAddress == BANK_2_BASE && native.codeLength != 0u) {
    state.warm_loader_return_allowed = 0;
    return H2_PAL_OK;
  }
  if (!state.confirmed_evidence_valid) return H2_PAL_ERR_INVALID_STATE;
  state.warm_loader_return_allowed = 0;
  return h2_jieli_upgrade_header_publish(state.confirmed_header) == 0
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
}

static int power_set_next(void* user, uint32_t partition_id) {
  (void)user;
  if (!state.configured) return H2_PAL_ERR_UNAVAILABLE;
  if (state.reboot_timer_id != 0u) return H2_PAL_ERR_INVALID_STATE;
  if (state.poisoned || h2_jieli_upgrade_erase_failed()) return H2_PAL_ERR_IO;
  if (partition_id != BANK_1_ID && partition_id != BANK_2_ID)
    return H2_PAL_ERR_NOT_FOUND;
  if (partition_id == state.running_partition_id) {
    /* Hard barrier: the caller may erase bank one as soon as this returns. */
    if (partition_id == BANK_2_ID) {
      int rc = publish_confirmed_bank_2();
      if (rc != H2_PAL_OK) return rc;
    }
    if (state.update_committed && state.committed_partition_id != 0u &&
        state.committed_partition_id != partition_id) {
      /* A failed reboot/transition may restore the previous running choice
       * after the inactive header was activated. Retain the running recovery
       * header first, then cancel only that inactive candidate's BootInfo. */
      int cleared = clear_update_boot_info();
      if (cleared != 0 || h2_jieli_upgrade_erase_failed()) return H2_PAL_ERR_IO;
      writer_abort_internal();
      if (state.poisoned) return H2_PAL_ERR_IO;
    }
    state.next_partition_id = partition_id;
    state.warm_partition_id = 0u;
    return H2_PAL_OK;
  }
  if (state.committed_partition_id == partition_id) {
    if (!state.prepared || state.update_committed)
      return H2_PAL_ERR_INVALID_STATE;
    if (state.update_active) {
      /* Canonical bank selection is allowed only after verified preparation. */
      int rc = native_burn(0);
      int exit_rc = native_exit();
      if (rc == H2_PAL_OK) rc = exit_rc;
      if (rc != H2_PAL_OK) {
        (void)image_writer_abort(NULL);
        return rc;
      }
    }
    state.update_committed = 1;
  } else if (state.update_active || state.writer_partition_id != 0u) {
    return H2_PAL_ERR_INVALID_STATE;
  } else if (partition_id == BANK_1_ID) {
    /* A fresh, consumed warm receipt proves ROM entered the canonical Loader.
     * Returning there needs a plain reset, never an invented bank-one remap.
     * Without that proof keep rejecting an ineffective selector change. */
    if (state.running_partition_id != BANK_2_ID ||
        !state.warm_loader_return_allowed)
      return H2_PAL_ERR_UNSUPPORTED;
  }
  /* A bank-two trial is entered through the warm remap; ROM remains on bank
   * one until a confirmed recovery image crosses the same-running barrier. */
  state.warm_partition_id = partition_id == BANK_2_ID ? partition_id : 0u;
  state.next_partition_id = partition_id;
  return H2_PAL_OK;
}

static void power_reboot_timer(void* user) {
  h2_jieli_wl82_diag_write("H2_JIELI_REBOOT_CALLBACK task=sys_timer\r\n");
  /* Publish only in the successfully scheduled callback immediately at reset.
   */
  if ((uintptr_t)user == BANK_2_BASE)
    h2_jieli_wl82_warm_boot_request(BANK_2_BASE);
  system_reset();
}

static int power_reboot(void* user, uint32_t reason) {
  (void)user;
  (void)reason;
  if (!state.configured) return H2_PAL_ERR_UNAVAILABLE;
  if (state.reboot_timer_id != 0u) return H2_PAL_OK;
  if (state.next_partition_id != state.running_partition_id &&
      state.committed_partition_id == state.next_partition_id &&
      !state.update_committed)
    return H2_PAL_ERR_INVALID_STATE;
  state.reboot_timer_id = sys_timeout_add_to_task(
      "sys_timer",
      (void*)(uintptr_t)(state.warm_partition_id == BANK_2_ID ? BANK_2_BASE
                                                              : 0u),
      power_reboot_timer, 2000u);
  return state.reboot_timer_id != 0u ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
}

h2_pal_result_t h2_jieli_wl82_platform_image_configure(
    const h2_jieli_wl82_image_config_t* config) {
  if (config == NULL || config->fs == NULL ||
      config->bank_1_partition_id == 0u || config->bank_2_partition_id == 0u ||
      config->bank_1_partition_id == config->bank_2_partition_id ||
      config->image_capacity == 0u || config->shadow_temp_path == NULL ||
      config->bank_1_shadow_path == NULL ||
      config->bank_2_shadow_path == NULL ||
      strlen(config->shadow_temp_path) >= 160u ||
      strlen(config->bank_1_shadow_path) >= 160u ||
      strlen(config->bank_2_shadow_path) >= 160u)
    return H2_PAL_ERR_INVALID_ARG;
  if (state.configured) {
    return state.config.fs == config->fs &&
                   state.config.bank_1_partition_id ==
                       config->bank_1_partition_id &&
                   state.config.bank_2_partition_id ==
                       config->bank_2_partition_id &&
                   state.config.image_capacity == config->image_capacity &&
                   strcmp(state.config.shadow_temp_path,
                          config->shadow_temp_path) == 0 &&
                   strcmp(state.config.bank_1_shadow_path,
                          config->bank_1_shadow_path) == 0 &&
                   strcmp(state.config.bank_2_shadow_path,
                          config->bank_2_shadow_path) == 0
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_STATE;
  }
  if (!state.callbacks_initialized) {
    h2_atomic_result_t rc = h2_atomic_bool_init(&state.burn_waiting, false);
    if (rc != H2_ATOMIC_OK)
      return rc == H2_ATOMIC_UNSUPPORTED ? H2_PAL_ERR_UNSUPPORTED
                                         : H2_PAL_ERR_NO_MEMORY;
    if (os_sem_create(&state.update_sem, 0) != OS_NO_ERR) {
      h2_atomic_bool_destroy(&state.burn_waiting);
      return H2_PAL_ERR_NO_MEMORY;
    }
    state.callbacks_initialized = 1;
  }
  struct BootInfo boot_info;
  memset(&boot_info, 0, sizeof(boot_info));
  int boot_info_rc = get_current_boot_info(&boot_info);
  uint32_t mapped_base =
      h2_jieli_warm_boot_running_base(boot_info_rc, boot_info.baseAddress);
  const h2_jieli_wl82_boot_geometry_t* geometry = h2_jieli_wl82_boot_geometry();
  if (geometry == NULL || mapped_base == 0u ||
      (mapped_base != geometry->bank_1_sfc_base &&
       mapped_base != geometry->bank_2_sfc_base))
    return H2_PAL_ERR_IO;
  state.config = *config;
  state.fs = config->fs;
  state.running_partition_id = mapped_base == geometry->bank_1_sfc_base
                                   ? config->bank_1_partition_id
                                   : config->bank_2_partition_id;
  state.next_partition_id = state.running_partition_id;
  state.warm_partition_id = mapped_base == geometry->bank_2_sfc_base
                                ? config->bank_2_partition_id
                                : 0u;
  const uint32_t rom_base = h2_jieli_warm_boot_take_rom_base();
  state.warm_loader_return_allowed = mapped_base == geometry->bank_2_sfc_base &&
                                     rom_base == geometry->bank_1_sfc_base;
  state.configured = 1;
  h2_jieli_warm_boot_report(h2_jieli_wl82_diag_write);
  return H2_PAL_OK;
}

const h2_pal_image_api_t* h2_jieli_wl82_platform_image_api(void) {
  static const h2_pal_image_vtable_t vtable = {
      .get_info = image_get_info,
      .read = image_read,
      .begin = image_writer_begin,
      .write = image_writer_write,
      .finish = image_writer_finish,
      .abort = image_writer_abort,
  };
  static const h2_pal_image_api_t api = {.vtable = &vtable};
  return &api;
}

const h2_pal_power_api_t* h2_jieli_wl82_platform_power_api(void) {
  static const h2_pal_power_vtable_t vtable = {
      .get_capabilities = power_get_capabilities,
      .list_boot_partitions = power_list,
      .get_running_boot_partition = power_get_running,
      .get_next_boot_partition = power_get_next,
      .set_next_boot_partition = power_set_next,
      .reboot = power_reboot,
      .prepare_next_boot_partition = power_prepare,
      .get_image_state = power_get_image_state,
      .confirm_running_image = power_confirm,
  };
  static const h2_pal_power_api_t api = {.vtable = &vtable};
  return &api;
}
