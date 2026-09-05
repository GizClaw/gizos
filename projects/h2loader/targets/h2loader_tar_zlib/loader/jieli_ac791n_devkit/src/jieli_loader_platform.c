#include "jieli_loader_platform.h"

#include "asm/includes.h"
#include "h2_jieli_ac791n_devkit_partitions.h"
#include "h2_loader_boot.h"
#include "os/os_api.h"
#include "timer.h"
#include "update/dual_bank_updata_api.h"

#include <string.h>

extern int snprintf(char *buffer, size_t size, const char *format, ...);

#define H2_JIELI_IMAGE_PATH_FORMAT "/data/.h2loader-image-%u"
#define H2_JIELI_IMAGE_TEMP_PATH "/data/.h2loader-image.tmp"
#define H2_JIELI_UPDATE_BLOCK_SIZE 4096u
#define H2_JIELI_UPDATE_WAIT_TICKS 500u
#define H2_JIELI_TRIAL_CHECKSUM_KEY "jieli_trial_checksum"
#define H2_JIELI_TRIAL_RESET_REASON_KEY "jieli_trial_reset_reason"

extern uint32_t get_target_udate_addr(void);
extern void h2_jieli_loader_diag_write(const char *text);

typedef struct h2_jieli_loader_platform {
  const h2_pal_fs_api_t *fs;
  uint32_t running_partition_id;
  uint32_t next_partition_id;
  uint32_t writer_partition_id;
  h2_pal_fs_file_t *shadow;
  h2_pal_fs_file_t *reader;
  uint32_t reader_partition_id;
  uint64_t reader_size;
  OS_SEM update_sem;
  uint8_t update_buffer[H2_JIELI_UPDATE_BLOCK_SIZE];
  size_t update_buffered;
  uint64_t update_native_written;
  uint64_t expected;
  uint64_t written;
  uint64_t committed_size;
  uint32_t committed_partition_id;
  int partition_2_shadow_reusable;
  int app_trial_rolled_back;
  int update_active;
  volatile int update_result;
  int update_committed;
  uint16_t reboot_timer_id;
} h2_jieli_loader_platform_t;

static h2_jieli_loader_platform_t state;

static void image_reader_close(void) {
  if (state.reader != NULL) {
    (void)h2_pal_fs_close(state.fs, state.reader);
    state.reader = NULL;
  }
  state.reader_partition_id = 0u;
  state.reader_size = 0u;
}

static void reconcile_trial_state(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator) {
  h2_loader_status_t status;
  if (h2_loader_read_pref_status(pref, allocator, &status) != H2_PAL_OK) {
    return;
  }
  h2_pal_pref_namespace_t *name_space = NULL;
  if (h2_pal_pref_open(
          pref, H2_LOADER_PREF_NAMESPACE, H2_PAL_PREF_OPEN_READ_WRITE,
          &name_space) != H2_PAL_OK) {
    return;
  }
  /* Once an App was committed, stage and Partition 2 describe that same
   * image. Seeing the canonical Loader execute again while both records are
   * still pending is authoritative rollback evidence, including failures
   * before the App can mount Pref or write its trial marker. Reinstalling in
   * that state creates an unrecoverable Loader/App reboot loop. */
  state.app_trial_rolled_back =
      state.running_partition_id == H2_JIELI_PARTITION_LOADER &&
      status.stage.valid && status.partition_2.valid &&
      status.partition_2.role == H2_LOADER_IMAGE_ROLE_APP &&
      h2_loader_metadata_image_equal(&status.stage, &status.partition_2);
  if (state.app_trial_rolled_back) {
    h2_jieli_loader_diag_write(
        "H2_JIELI_TRIAL_ROLLBACK app_bootable=0 action=command-mode\r\n");
  }
  uint32_t reset_reason = 0u;
  int reason_result = name_space->get_u32(
      name_space, H2_JIELI_TRIAL_RESET_REASON_KEY, &reset_reason);
  if (reason_result == H2_PAL_OK) {
    char line[96];
    (void)snprintf(
        line, sizeof(line),
        "H2_JIELI_TRIAL_ROLLBACK reset_reason=0x%x\r\n",
        (unsigned)reset_reason);
    h2_jieli_loader_diag_write(line);
  }
  if (!status.stage.valid || !status.partition_2.valid ||
      status.partition_2.role != H2_LOADER_IMAGE_ROLE_APP ||
      !h2_loader_metadata_image_equal(&status.stage, &status.partition_2)) {
    int result = name_space->remove(
        name_space, H2_JIELI_TRIAL_CHECKSUM_KEY);
    if (result == H2_PAL_ERR_NOT_FOUND) result = H2_PAL_OK;
    int reason_remove = name_space->remove(
        name_space, H2_JIELI_TRIAL_RESET_REASON_KEY);
    if (reason_remove == H2_PAL_ERR_NOT_FOUND) reason_remove = H2_PAL_OK;
    if (result == H2_PAL_OK) result = reason_remove;
    if (result == H2_PAL_OK && name_space->commit != NULL) {
      (void)name_space->commit(name_space);
    }
  }
  if (name_space->close != NULL) (void)name_space->close(name_space);
}

static int image_path(
    uint32_t partition_id, char *out_path, size_t out_path_size) {
  if (partition_id != H2_JIELI_PARTITION_LOADER &&
      partition_id != H2_JIELI_PARTITION_APP) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  int length = snprintf(
      out_path, out_path_size, H2_JIELI_IMAGE_PATH_FORMAT,
      (unsigned)partition_id);
  return length > 0 && (size_t)length < out_path_size
             ? H2_PAL_OK
             : H2_PAL_ERR_NO_SPACE;
}

static void writer_abort_internal(void) {
  image_reader_close();
  if (state.update_active) {
    (void)dual_bank_passive_update_exit(NULL);
    (void)os_sem_del(&state.update_sem, 0);
    state.update_active = 0;
  }
  if (state.shadow != NULL) {
    (void)h2_pal_fs_close(state.fs, state.shadow);
    state.shadow = NULL;
  }
  if (state.fs != NULL) {
    (void)h2_pal_fs_remove(state.fs, H2_JIELI_IMAGE_TEMP_PATH);
    if (state.committed_partition_id != 0u && !state.update_committed) {
      char path[48];
      if (image_path(
              state.committed_partition_id, path, sizeof(path)) ==
          H2_PAL_OK) {
        (void)h2_pal_fs_remove(state.fs, path);
      }
      if (state.committed_partition_id == H2_JIELI_PARTITION_APP) {
        state.partition_2_shadow_reusable = 0;
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
}

static int image_get_capacity(
    void *user, uint32_t partition_id, uint64_t *out_capacity) {
  (void)user;
  if (out_capacity == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (partition_id == H2_JIELI_PARTITION_LOADER ||
      partition_id == H2_JIELI_PARTITION_APP) {
    /* These are logical A/B roles. JieLi's packer owns the physical bank
     * placement; CODE_BOUNDARY_LINE is the maximum image size of either
     * executable bank. */
    *out_capacity = H2_JIELI_IMAGE_MAX_SIZE;
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static int image_read(
    void *user, uint32_t partition_id, uint64_t offset, void *data,
    size_t len) {
  char line[144];
  char path[48];
  h2_pal_fs_stat_t stat_value;
  size_t read = 0u;
  (void)user;
  if ((data == NULL && len != 0u) || state.fs == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  /* JieLi cannot non-destructively select an already-programmed inactive
   * App bank.  An App shadow is therefore reusable only during the writer
   * transaction that can still commit BootInfo.  A Loader shadow is reusable
   * across the self-upgrade reboot because the second Loader must copy it
   * back into the canonical bank. */
  if (partition_id == H2_JIELI_PARTITION_APP &&
      !state.partition_2_shadow_reusable &&
      state.committed_partition_id != H2_JIELI_PARTITION_APP) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  /* JieLi's passive updater transforms the UFW byte stream before storing it
   * in the inactive flash bank, so a physical-bank SHA is not comparable to
   * the manifest SHA. The persistent SD shadow preserves the exact input
   * bytes across the trial and canonical reboots and is the canonical reader
   * for common Loader verification/copy. A stale shadow is harmless because
   * the common layer verifies its size and SHA against the persisted upgrade
   * record before selecting either bank. */
  int rc = image_path(partition_id, path, sizeof(path));
  const char *step = "path";
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
      rc = H2_PAL_ERR_NOT_FOUND;
    }
    if (rc == H2_PAL_OK) {
      step = "open";
      rc = h2_pal_fs_open(
          state.fs, path, H2_PAL_FS_OPEN_READ, &state.reader);
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
  if (rc == H2_PAL_OK &&
      (offset == 0u || offset + len == state.reader_size)) {
    (void)snprintf(
        line, sizeof(line),
        "H2_JIELI_IMAGE_READ_SHADOW partition=%u offset=%u bytes=%u\r\n",
        (unsigned)partition_id, (unsigned)offset, (unsigned)len);
    h2_jieli_loader_diag_write(line);
  }
  if (rc != H2_PAL_OK) {
    (void)snprintf(
        line, sizeof(line),
        "H2_JIELI_IMAGE_READ_SD_ERROR step=%s partition=%u offset=%u bytes=%u read=%u rc=%d\r\n",
        step, (unsigned)partition_id, (unsigned)offset,
        (unsigned)len, (unsigned)read, rc);
    h2_jieli_loader_diag_write(line);
  }
  if (rc != H2_PAL_OK || offset + len == state.reader_size) {
    image_reader_close();
  }
  return rc;
}

static int image_writer_begin(
    void *user, uint32_t partition_id,
    const h2_loader_image_identity_t *identity) {
  char line[128];
  uint64_t capacity = 0u;
  (void)user;
  h2_jieli_loader_diag_write("H2_JIELI_UPDATE_WRITER_ENTER\r\n");
  writer_abort_internal();
  if (identity == NULL || identity->image_size == 0u ||
      identity->image_size > UINT32_MAX ||
      partition_id == state.running_partition_id) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int rc = image_get_capacity(NULL, partition_id, &capacity);
  if (rc != H2_PAL_OK || identity->image_size > capacity) {
    return rc == H2_PAL_OK ? H2_PAL_ERR_NO_SPACE : rc;
  }
  rc = h2_pal_fs_open(
      state.fs, H2_JIELI_IMAGE_TEMP_PATH,
      H2_PAL_FS_OPEN_WRITE_TRUNCATE, &state.shadow);
  if (rc != H2_PAL_OK) return rc;
  if (os_sem_create(&state.update_sem, 0) != OS_NO_ERR) {
    writer_abort_internal();
    return H2_PAL_ERR_IO;
  }
  state.update_active = 1;
  uint32_t image_size = (uint32_t)identity->image_size;
  /* This writer consumes JieLi's self-describing UFW stream.  Match
   * net_update.c and pass zero CRC/size so the native updater parses the UFW
   * header.  Non-zero CRC/size selects the raw-firmware mode used by TWS. */
  h2_jieli_loader_diag_write("H2_JIELI_UPDATE_INIT_ENTER\r\n");
  uint32_t init_rc = dual_bank_passive_update_init(
      0u, 0u, H2_JIELI_UPDATE_BLOCK_SIZE, NULL);
  (void)snprintf(line, sizeof(line),
      "H2_JIELI_UPDATE_INIT_RETURN rc=%u\r\n", (unsigned)init_rc);
  h2_jieli_loader_diag_write(line);
  uint32_t allow_rc =
      init_rc == 0u ? dual_bank_update_allow_check(0u) : 0u;
  (void)snprintf(
      line, sizeof(line),
      "H2_JIELI_UPDATE_BEGIN bytes=%u init=%u allow=%u\r\n",
      (unsigned)image_size, (unsigned)init_rc, (unsigned)allow_rc);
  h2_jieli_loader_diag_write(line);
  if (init_rc != 0u || allow_rc != 0u) {
    writer_abort_internal();
    (void)flash_update_clr_boot_info(CLEAR_APP_UPDATE_BANK);
    return H2_PAL_ERR_NO_SPACE;
  }
  state.writer_partition_id = partition_id;
  state.update_buffered = 0u;
  state.update_native_written = 0u;
  state.update_result = H2_PAL_OK;
  state.expected = identity->image_size;
  state.written = 0u;
  state.update_committed = 0;
  return H2_PAL_OK;
}

static int update_write_error(void *result) {
  /* Match JieLi's net_update implementation: this callback reports only an
   * asynchronous programming error.  A successful write does not call it. */
  if (result != NULL) state.update_result = H2_PAL_ERR_IO;
  return 0;
}

static int update_write_block(
    const uint8_t *data, size_t len, int report_async_errors) {
  char line[112];
  if (len == 0u || len > UINT16_MAX) return H2_PAL_ERR_INVALID_ARG;
  if (state.update_result != H2_PAL_OK) return state.update_result;
  const int trace = state.update_native_written >= 512u * 1024u;
  if (trace) {
    (void)snprintf(line, sizeof(line),
        "H2_JIELI_UPDATE_BLOCK_ENTER offset=%u bytes=%u\r\n",
        (unsigned)state.update_native_written, (unsigned)len);
    h2_jieli_loader_diag_write(line);
  }
  uint32_t native_rc = dual_bank_update_write(
      (void *)data, (uint16_t)len,
      report_async_errors ? update_write_error : NULL);
  if (trace) {
    (void)snprintf(line, sizeof(line),
        "H2_JIELI_UPDATE_BLOCK_RETURN offset=%u bytes=%u rc=%u\r\n",
        (unsigned)state.update_native_written, (unsigned)len,
        (unsigned)native_rc);
    h2_jieli_loader_diag_write(line);
  }
  if (native_rc != 0u) {
    (void)snprintf(
        line, sizeof(line),
        "H2_JIELI_UPDATE_WRITE_ERROR offset=%u bytes=%u native=%u\r\n",
        (unsigned)state.update_native_written, (unsigned)len,
        (unsigned)native_rc);
    h2_jieli_loader_diag_write(line);
    return H2_PAL_ERR_IO;
  }
  return state.update_result;
}

static int update_flush_buffer(void) {
  size_t buffered = state.update_buffered;
  if (state.update_native_written == 0u) {
    uint32_t target = get_target_udate_addr();
    uint32_t aligned =
        (target + H2_JIELI_UPDATE_BLOCK_SIZE - 1u) &
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
        memmove(
            state.update_buffer, state.update_buffer + first,
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

static int update_burn_complete(int error) {
  char line[96];
  state.update_result = error == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
  (void)snprintf(
      line, sizeof(line), "H2_JIELI_UPDATE_BURN_CALLBACK error=%d\r\n", error);
  h2_jieli_loader_diag_write(line);
  os_sem_post(&state.update_sem);
  return 0;
}

static int image_writer_write(void *user, const void *data, size_t len) {
  size_t shadow_written = 0u;
  (void)user;
  if (!state.update_active || state.shadow == NULL ||
      (data == NULL && len != 0u) ||
      state.written > state.expected || len > state.expected - state.written) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (len == 0u) return H2_PAL_OK;
  int rc = h2_pal_fs_write(
      state.fs, state.shadow, data, len, &shadow_written);
  if (rc != H2_PAL_OK || shadow_written != len) {
    writer_abort_internal();
    return rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc;
  }
  const uint8_t *cursor = data;
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
        (void)flash_update_clr_boot_info(CLEAR_APP_UPDATE_BANK);
        return rc;
      }
    }
  }
  state.written += len;
  return H2_PAL_OK;
}

static int image_writer_finish(
    void *user, const h2_loader_image_identity_t *identity) {
  char line[128];
  char final_path[48];
  uint32_t partition_id = state.writer_partition_id;
  (void)user;
  h2_jieli_loader_diag_write("H2_JIELI_UPDATE_FINISH_ENTER\r\n");
  if (identity == NULL || !state.update_active ||
      state.shadow == NULL || state.written != state.expected ||
      identity->image_size != state.expected) {
    writer_abort_internal();
    return H2_PAL_ERR_INVALID_STATE;
  }
  int rc = update_flush_buffer();
  if (rc == H2_PAL_OK && state.update_result != H2_PAL_OK) {
    rc = state.update_result;
  }
  (void)snprintf(
      line, sizeof(line),
      "H2_JIELI_UPDATE_WRITE_DONE expected=%u native=%u result=%d\r\n",
      (unsigned)state.expected, (unsigned)state.update_native_written,
      state.update_result);
  h2_jieli_loader_diag_write(line);
  if (rc == H2_PAL_OK) rc = h2_pal_fs_sync(state.fs, state.shadow);
  int close_rc = h2_pal_fs_close(state.fs, state.shadow);
  state.shadow = NULL;
  if (rc == H2_PAL_OK) rc = close_rc;
  if (rc == H2_PAL_OK) rc = image_path(partition_id, final_path, sizeof(final_path));
  if (rc == H2_PAL_OK) {
    (void)h2_pal_fs_remove(state.fs, final_path);
    rc = h2_pal_fs_rename(
        state.fs, H2_JIELI_IMAGE_TEMP_PATH, final_path);
  }
  if (rc != H2_PAL_OK) {
    (void)flash_update_clr_boot_info(CLEAR_APP_UPDATE_BANK);
    writer_abort_internal();
    return rc;
  }
  /* Keep the native update session open while the common Loader hashes the
   * destination bank. Boot Info is committed later by power_set_next(), only
   * after package verification and preference updates have succeeded. */
  state.committed_partition_id = partition_id;
  state.committed_size = identity->image_size;
  if (partition_id == H2_JIELI_PARTITION_APP) {
    state.partition_2_shadow_reusable = 1;
  }
  state.writer_partition_id = 0u;
  state.expected = 0u;
  state.written = 0u;
  return H2_PAL_OK;
}

static void image_writer_abort(void *user) {
  (void)user;
  writer_abort_internal();
  (void)flash_update_clr_boot_info(CLEAR_APP_UPDATE_BANK);
}

static int power_get_capabilities(
    void *user, h2_pal_power_capabilities_t *out_capabilities) {
  (void)user;
  if (out_capabilities == NULL) return H2_PAL_ERR_INVALID_ARG;
  out_capabilities->flags =
      H2_PAL_POWER_CAPABILITY_REBOOT |
      H2_PAL_POWER_CAPABILITY_BOOT_PARTITIONS |
      H2_PAL_POWER_CAPABILITY_SET_NEXT_BOOT_PARTITION;
  return H2_PAL_OK;
}

static void fill_partition(
    h2_pal_power_boot_partition_t *partition, uint32_t id,
    uint32_t flags) {
  memset(partition, 0, sizeof(*partition));
  partition->id = id;
  partition->flags = H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE | flags;
  if (id == H2_JIELI_PARTITION_APP && state.app_trial_rolled_back) {
    partition->flags &= ~H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE;
  }
  (void)snprintf(
      partition->name, sizeof(partition->name), "%s",
      id == H2_JIELI_PARTITION_LOADER ? "h2loader" : "app");
}

static int power_list(
    void *user, h2_pal_power_boot_partition_cb_t callback,
    void *callback_user) {
  h2_pal_power_boot_partition_t partition;
  (void)user;
  if (callback == NULL) return H2_PAL_ERR_INVALID_ARG;
  fill_partition(
      &partition, H2_JIELI_PARTITION_LOADER,
      H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY);
  int rc = callback(callback_user, &partition);
  if (rc != H2_PAL_OK) return rc;
  fill_partition(
      &partition, H2_JIELI_PARTITION_APP,
      H2_PAL_POWER_BOOT_PARTITION_FLAG_APP);
  return callback(callback_user, &partition);
}

static int power_get_running(
    void *user, h2_pal_power_boot_partition_t *out_partition) {
  (void)user;
  if (out_partition == NULL || state.running_partition_id == 0u) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  fill_partition(
      out_partition, state.running_partition_id,
      H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING |
          (state.running_partition_id == H2_JIELI_PARTITION_LOADER
               ? H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY
               : H2_PAL_POWER_BOOT_PARTITION_FLAG_APP));
  return H2_PAL_OK;
}

static int power_get_next(
    void *user, h2_pal_power_boot_partition_t *out_partition) {
  (void)user;
  if (out_partition == NULL || state.next_partition_id == 0u) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  fill_partition(
      out_partition, state.next_partition_id,
      H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT);
  return H2_PAL_OK;
}

static int power_set_next(void *user, uint32_t partition_id) {
  char line[128];
  (void)user;
  if (state.reboot_timer_id != 0u) return H2_PAL_ERR_INVALID_STATE;
  (void)snprintf(
      line, sizeof(line),
      "H2_JIELI_SET_NEXT requested=%u running=%u active=%d committed=%d "
      "candidate=%u bytes=%u\r\n",
      (unsigned)partition_id, (unsigned)state.running_partition_id,
      state.update_active, state.update_committed,
      (unsigned)state.committed_partition_id,
      (unsigned)state.committed_size);
  h2_jieli_loader_diag_write(line);
  if (partition_id != H2_JIELI_PARTITION_LOADER &&
      partition_id != H2_JIELI_PARTITION_APP) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (partition_id == state.running_partition_id) {
    if (state.update_committed) {
      int rc = flash_update_clr_boot_info(CLEAR_APP_UPDATE_BANK) == 0
                   ? H2_PAL_OK
                   : H2_PAL_ERR_IO;
      if (rc != H2_PAL_OK) return rc;
      state.update_committed = 0;
      state.committed_partition_id = 0u;
      state.committed_size = 0u;
    }
    state.next_partition_id = partition_id;
    return H2_PAL_OK;
  }
  if (!state.update_active || state.update_committed ||
      state.committed_partition_id != partition_id) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  state.update_result = H2_PAL_ERR_TIMEOUT;
  h2_jieli_loader_diag_write("H2_JIELI_UPDATE_BURN_ENTER\r\n");
  uint32_t burn_rc = dual_bank_update_burn_boot_info(update_burn_complete);
  (void)snprintf(
      line, sizeof(line), "H2_JIELI_UPDATE_BURN_RETURN call=%u\r\n",
      (unsigned)burn_rc);
  h2_jieli_loader_diag_write(line);
  int pend_rc = OS_NO_ERR;
  int rc;
  if (burn_rc != 0u) {
    rc = H2_PAL_ERR_IO;
  } else if (
      (pend_rc = os_sem_pend(
           &state.update_sem, H2_JIELI_UPDATE_WAIT_TICKS)) != OS_NO_ERR) {
    rc = H2_PAL_ERR_TIMEOUT;
  } else {
    rc = state.update_result;
  }
  (void)snprintf(
      line, sizeof(line),
      "H2_JIELI_UPDATE_BURN call=%u pend=%d result=%d\r\n",
      (unsigned)burn_rc, pend_rc, rc);
  h2_jieli_loader_diag_write(line);
  if (state.update_active) {
    h2_jieli_loader_diag_write("H2_JIELI_UPDATE_EXIT_ENTER\r\n");
    int exit_rc = dual_bank_passive_update_exit(NULL) == 0u
                      ? H2_PAL_OK
                      : H2_PAL_ERR_IO;
    (void)os_sem_del(&state.update_sem, 0);
    state.update_active = 0;
    if (rc == H2_PAL_OK) rc = exit_rc;
  }
  if (rc != H2_PAL_OK) {
    (void)flash_update_clr_boot_info(CLEAR_APP_UPDATE_BANK);
    writer_abort_internal();
    return rc;
  }
  state.update_committed = 1;
  state.app_trial_rolled_back = 0;
  state.next_partition_id = partition_id;
  return H2_PAL_OK;
}

static void power_reboot_timer(void *user) {
  (void)user;
  h2_jieli_loader_diag_write("H2_JIELI_REBOOT_CALLBACK task=sys_timer\r\n");
  h2_jieli_loader_diag_write("H2_JIELI_REBOOT_EXECUTE reset=core\r\n");
  system_reset();
}

static int power_reboot(void *user, uint32_t reason) {
  (void)user;
  (void)reason;
  if (state.next_partition_id != 0u &&
      state.next_partition_id != state.running_partition_id &&
      !state.update_committed) {
    /* JieLi exposes no non-destructive selector for an already-programmed
     * inactive bank. Cross-bank boot is therefore valid only immediately
     * after this PAL committed a new bank. Never erase the running Loader. */
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (state.reboot_timer_id != 0u) return H2_PAL_OK;
  char line[128];
  (void)snprintf(
      line, sizeof(line),
      "H2_JIELI_POWER_REBOOT running=%u next=%u committed=%d reason=%u\r\n",
      (unsigned)state.running_partition_id,
      (unsigned)state.next_partition_id,
      state.update_committed, (unsigned)reason);
  h2_jieli_loader_diag_write(line);
  /* Match the SDK net_update.c lifecycle: defer reset to sys_timer for 2 s
   * after committing the bank, rather than resetting inline in the Loader
   * task while the native updater and command response are being retired. */
  state.reboot_timer_id = sys_timeout_add_to_task(
      "sys_timer", NULL, power_reboot_timer, 2000u);
  (void)snprintf(
      line, sizeof(line), "H2_JIELI_REBOOT_SCHEDULED timer=%u delay_ms=2000\r\n",
      (unsigned)state.reboot_timer_id);
  h2_jieli_loader_diag_write(line);
  if (state.reboot_timer_id == 0u) return H2_PAL_ERR_NO_MEMORY;
  return H2_PAL_OK;
}

int h2_jieli_loader_platform_init(
    const h2_pal_fs_api_t *fs, const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator) {
  if (fs == NULL || pref == NULL || allocator == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(&state, 0, sizeof(state));
  state.fs = fs;
  h2_loader_status_t loader_status;
  memset(&loader_status, 0, sizeof(loader_status));
  if (h2_loader_read_pref_status(pref, allocator, &loader_status) ==
          H2_PAL_OK &&
      loader_status.partition_2.valid &&
      loader_status.partition_2.role == H2_LOADER_IMAGE_ROLE_H2LOADER) {
    state.partition_2_shadow_reusable = 1;
  }
  /* The v2 common Loader models both native banks directly as Partition 1
   * and Partition 2.  JieLi's current BootInfo base address is the durable
   * source of truth for which physical bank is executing; no separate
   * self-upgrade phase record is needed. */
  struct BootInfo boot_info;
  memset(&boot_info, 0, sizeof(boot_info));
  int boot_info_rc = get_current_boot_info(&boot_info);
  state.running_partition_id =
      boot_info_rc == 0 &&
              boot_info.baseAddress >= H2_JIELI_IMAGE_MAX_SIZE / 2u
          ? H2_JIELI_PARTITION_APP
          : H2_JIELI_PARTITION_LOADER;
  state.next_partition_id = state.running_partition_id;
  char line[192];
  (void)snprintf(
      line, sizeof(line),
      "H2_JIELI_BOOT_INFO result=%d base=0x%x bytes=%u version=%u "
      "logical=%u\r\n",
      boot_info_rc, (unsigned)boot_info.baseAddress,
      (unsigned)boot_info.codeLength, (unsigned)boot_info.version,
      (unsigned)state.running_partition_id);
  h2_jieli_loader_diag_write(line);
  reconcile_trial_state(pref, allocator);
  return H2_PAL_OK;
}

const h2_pal_power_api_t *h2_jieli_loader_power_api(void) {
  static const h2_pal_power_vtable_t vtable = {
      .get_capabilities = power_get_capabilities,
      .list_boot_partitions = power_list,
      .get_running_boot_partition = power_get_running,
      .get_next_boot_partition = power_get_next,
      .set_next_boot_partition = power_set_next,
      .reboot = power_reboot,
  };
  static const h2_pal_power_api_t api = {.vtable = &vtable};
  return &api;
}

const h2_loader_image_reader_api_t *h2_jieli_loader_image_reader(void) {
  static const h2_loader_image_reader_vtable_t vtable = {
      .get_capacity = image_get_capacity,
      .read = image_read,
  };
  static const h2_loader_image_reader_api_t api = {.vtable = &vtable};
  return &api;
}

const h2_loader_image_writer_api_t *h2_jieli_loader_image_writer(void) {
  static const h2_loader_image_writer_vtable_t vtable = {
      .get_capacity = image_get_capacity,
      .begin = image_writer_begin,
      .write = image_writer_write,
      .finish = image_writer_finish,
      .abort = image_writer_abort,
  };
  static const h2_loader_image_writer_api_t api = {.vtable = &vtable};
  return &api;
}

int h2_jieli_loader_confirm_active_image(void *user) {
  (void)user;
  return H2_PAL_OK;
}
