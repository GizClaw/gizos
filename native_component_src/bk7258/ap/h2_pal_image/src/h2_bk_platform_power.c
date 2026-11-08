#include "h2_bk_platform_image.h"
#include "h2_bk_image_internal.h"
#ifdef H2_BK_H2LOADER_POWER_HOST_TEST
#include "h2_bk_h2loader_power_test_sdk.h"
#else
#include "h2_bk_fixed_boot.h"
#include "h2_bk_flash_access.h"
#include "h2_bk_platform_storage.h"
#include "flash_access.h"
#include "bk_private/bk_ota_private.h"
#include "components/system.h"
#include "driver/flash.h"
#include "driver/flash_partition.h"
#include "driver/wdt.h"
#include "modules/ota.h"
#include "wdt_driver.h"
#endif
#include <stdio.h>
#include <string.h>

extern part_flag update_part_flag;

static uint32_t s_prepared_partition;
static h2_pal_power_boot_policy_t s_policy;

static int write_execution_flags(uint8_t final, uint8_t temporary,
                                 uint8_t confirm) {
  const bk_logic_partition_t* p =
      bk_flash_partition_get_info(BK_PARTITION_OTA_FINA_EXECUTIVE);
  if (!p) return H2_PAL_ERR_IO;
  uint8_t flags[12];
  memset(flags, 255, sizeof(flags));
  flags[0] = final;
  flags[4] = temporary;
  flags[8] = confirm;
  uint8_t current[sizeof(flags)];
  if (bk_flash_read_bytes(p->partition_start_addr, current, sizeof(current)) !=
      BK_OK)
    return H2_PAL_ERR_IO;
  if (memcmp(current, flags, sizeof(flags)) == 0) return H2_PAL_OK;
  flash_protect_type_t protect = bk_flash_get_protect_type();
  int rc = bk_flash_set_protect_type(FLASH_PROTECT_NONE);
  if (rc == BK_OK) rc = bk_flash_erase_sector(p->partition_start_addr);
  if (rc == BK_OK)
    rc = bk_flash_write_bytes(p->partition_start_addr, flags, sizeof(flags));
  (void)bk_flash_set_protect_type(protect);
  printf(
      "H2_BK_OTA_WRITER stage=write_flags rc=%d final=%u temp=%u confirm=%u\r\n",
      rc, (unsigned) final, (unsigned)temporary, (unsigned)confirm);
  return rc == BK_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t power_get_capabilities(
    void* user, h2_pal_power_capabilities_t* out_capabilities) {
  (void)user;
  if (out_capabilities == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_capabilities->flags = H2_PAL_POWER_CAPABILITY_REBOOT |
                            H2_PAL_POWER_CAPABILITY_BOOT_PARTITIONS |
                            H2_PAL_POWER_CAPABILITY_SET_NEXT_BOOT_PARTITION |
                            H2_PAL_POWER_CAPABILITY_IMAGE_STATE |
                            H2_PAL_POWER_CAPABILITY_PREPARE_BOOT |
                            H2_PAL_POWER_CAPABILITY_CONFIRM_IMAGE |
                            H2_PAL_POWER_CAPABILITY_WATCHDOG_FEED;
  return H2_PAL_OK;
}

static void fill_partition(h2_pal_power_boot_partition_t* out_partition,
                           uint32_t id, const char* name, uint32_t flags) {
  memset(out_partition, 0, sizeof(*out_partition));
  out_partition->id = id;
  out_partition->flags = flags;
  (void)snprintf(out_partition->name, sizeof(out_partition->name), "%s", name);
}

static h2_pal_result_t power_list_boot_partitions(
    void* user, h2_pal_power_boot_partition_cb_t cb, void* cb_user) {
  h2_pal_power_boot_partition_t partition;
  h2_pal_result_t rc;
  const bk_logic_partition_t* control;
  uint8_t flags[9];
  uint32_t app_flags = H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
                       H2_PAL_POWER_BOOT_PARTITION_FLAG_APP;

  (void)user;
  if (cb == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  control = bk_flash_partition_get_info(BK_PARTITION_OTA_FINA_EXECUTIVE);
  if (control == NULL || bk_flash_read_bytes(control->partition_start_addr,
                                             flags, sizeof(flags)) != BK_OK) {
    return H2_PAL_ERR_IO;
  }
  /* App startup arms A as recovery while retaining B as the attempted
   * image. On returning to A, this exact flag combination proves that B
   * never confirmed. Explicit Loader selection writes A/A instead. */
  if ((h2_bk_fixed_layout_active() && h2_bk_fixed_current_slot() == 0u &&
       (h2_bk_fixed_app_failed() || h2_bk_fixed_relay_failed())) ||
      (!h2_bk_fixed_layout_active() &&
       h2_bk_fixed_current_slot() == EXEX_A_PART && flags[0] == EXEX_A_PART &&
       flags[4] == EXEC_B_PART && flags[8] == CONFIRM_EXEC_A)) {
    app_flags &= ~H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE;
  }
  fill_partition(&partition, H2_BK_PRIMARY_PARTITION_ID, "primary_loader",
                 H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
                     H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY);
  rc = cb(cb_user, &partition);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  fill_partition(&partition, H2_BK_APP_PARTITION_ID, "s_app", app_flags);
  return cb(cb_user, &partition);
}

static h2_pal_result_t power_get_running_boot_partition(
    void* user, h2_pal_power_boot_partition_t* out_partition) {
  uint8_t current;

  (void)user;
  if (out_partition == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  current = h2_bk_fixed_current_slot();
  if (current == 0u) {
    fill_partition(out_partition, H2_BK_PRIMARY_PARTITION_ID, "primary_loader",
                   H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
                       H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING |
                       H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY);
  } else {
    fill_partition(out_partition, H2_BK_APP_PARTITION_ID, "s_app",
                   H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
                       H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING |
                       H2_PAL_POWER_BOOT_PARTITION_FLAG_APP);
  }
  return H2_PAL_OK;
}

static h2_pal_result_t power_get_next_boot_partition(
    void* user, h2_pal_power_boot_partition_t* out_partition) {
  const bk_logic_partition_t* control;
  uint8_t flags[9];
  uint8_t next;

  if (out_partition == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (h2_bk_fixed_layout_active()) {
    int app = h2_bk_fixed_next_app();
    fill_partition(out_partition,
                   app ? H2_BK_APP_PARTITION_ID : H2_BK_PRIMARY_PARTITION_ID,
                   app ? "fixed_app" : "primary_loader",
                   H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
                       H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT |
                       (app ? H2_PAL_POWER_BOOT_PARTITION_FLAG_APP
                            : H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY));
    return H2_PAL_OK;
  }
  control = bk_flash_partition_get_info(BK_PARTITION_OTA_FINA_EXECUTIVE);
  if (control == NULL || bk_flash_read_bytes(control->partition_start_addr,
                                             flags, sizeof(flags)) != BK_OK) {
    return H2_PAL_ERR_IO;
  }
  /* OTA flag 1 requests the temporary partition; otherwise use the
   * confirmed selection. An erased control record keeps the current boot. */
  next = flags[8] == 1u ? flags[4] : flags[0];
  if (flags[0] == 0xffu && flags[4] == 0xffu && flags[8] == 0xffu) {
    h2_pal_result_t rc = power_get_running_boot_partition(user, out_partition);
    if (rc == H2_PAL_OK) {
      out_partition->flags &= ~H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING;
      out_partition->flags |= H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT;
    }
    return rc;
  }
  if (next != EXEX_A_PART && next != EXEC_B_PART)
    return H2_PAL_ERR_INVALID_STATE;
  fill_partition(
      out_partition,
      next == EXEX_A_PART ? H2_BK_PRIMARY_PARTITION_ID : H2_BK_APP_PARTITION_ID,
      next == EXEX_A_PART ? "primary_loader" : "s_app",
      H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
          H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT |
          (next == EXEX_A_PART ? H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY
                               : H2_PAL_POWER_BOOT_PARTITION_FLAG_APP));
  return H2_PAL_OK;
}

static h2_pal_result_t power_get_image_state(void* user, uint32_t id,
                                             h2_pal_power_image_state_t* out) {
  (void)user;
  if (!out ||
      (id != H2_BK_PRIMARY_PARTITION_ID && id != H2_BK_APP_PARTITION_ID))
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (h2_bk_image_prepared(id)) {
    out->state = H2_PAL_POWER_IMAGE_STATE_NEW;
    out->flags = H2_PAL_POWER_IMAGE_FLAG_CONFIRMATION_REQUIRED;
    uint32_t execution;
    if (h2_bk_image_execution_partition(id, &execution) == H2_PAL_OK &&
        id != execution)
      out->flags |= H2_PAL_POWER_IMAGE_FLAG_RECOVERY_REQUIRED;
    return H2_PAL_OK;
  }
  if (h2_bk_fixed_layout_active()) return h2_bk_fixed_image_state(id, out);
  const bk_logic_partition_t* p =
      bk_flash_partition_get_info(BK_PARTITION_OTA_FINA_EXECUTIVE);
  uint8_t flags[9];
  if (!p || bk_flash_read_bytes(p->partition_start_addr, flags,
                                sizeof(flags)) != BK_OK)
    return H2_PAL_ERR_IO;
  const int running = h2_bk_fixed_current_slot() == id - 1u;
  if (flags[4] == id - 1u && flags[8] == 1u) {
    out->state = H2_PAL_POWER_IMAGE_STATE_PENDING_VERIFY;
    out->flags = H2_PAL_POWER_IMAGE_FLAG_CONFIRMATION_REQUIRED;
    if (!running && flags[0] != id - 1u) {
      out->state = H2_PAL_POWER_IMAGE_STATE_ABORTED;
      out->flags |= H2_PAL_POWER_IMAGE_FLAG_UNCONFIRMED_ATTEMPT;
    }
  } else if (id == H2_BK_APP_PARTITION_ID && flags[0] == 0u && flags[4] == 1u &&
             flags[8] == CONFIRM_EXEC_A) {
    out->state = running ? H2_PAL_POWER_IMAGE_STATE_PENDING_VERIFY
                         : H2_PAL_POWER_IMAGE_STATE_ABORTED;
    out->flags = H2_PAL_POWER_IMAGE_FLAG_CONFIRMATION_REQUIRED |
                 H2_PAL_POWER_IMAGE_FLAG_UNCONFIRMED_ATTEMPT;
  } else {
    out->state = running || (flags[0] == id - 1u && flags[8] == id + 2u)
                     ? H2_PAL_POWER_IMAGE_STATE_VALID
                     : H2_PAL_POWER_IMAGE_STATE_NEW;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t power_prepare(void* user,
                                     const h2_pal_power_boot_request_t* request,
                                     h2_pal_power_native_evidence_t* evidence) {
  (void)user;
  if (!request || !evidence ||
      (request->partition_id != H2_BK_PRIMARY_PARTITION_ID &&
       request->partition_id != H2_BK_APP_PARTITION_ID) ||
      request->policy > H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY)
    return H2_PAL_ERR_INVALID_ARG;
  memset(evidence, 0, sizeof(*evidence));
  if (h2_bk_image_write_active()) return H2_PAL_ERR_BUSY;
  /* Validate the fixed App execution window before preparing selection.
   * Selecting the installed canonical Loader requires no App vector read.
   * Legacy SDK A/B retains its SDK-owned remap/copy behaviour. */
  if (h2_bk_fixed_layout_active() &&
      request->partition_id == H2_BK_APP_PARTITION_ID) {
    uint32_t execution = 0;
    int rc = h2_bk_image_execution_partition(request->partition_id, &execution);
    if (rc != H2_PAL_OK) return rc;
  }
  /* Empty evidence: the native control records survive reset. */
  s_prepared_partition = request->partition_id;
  s_policy = request->policy;
  return H2_PAL_OK;
}

static h2_pal_result_t power_set_next_boot_partition(void* user, uint32_t id) {
  (void)user;
  if (id != H2_BK_PRIMARY_PARTITION_ID && id != H2_BK_APP_PARTITION_ID)
    return H2_PAL_ERR_INVALID_ARG;
  if (h2_bk_image_write_active()) return H2_PAL_ERR_BUSY;
  /* Old callers may select an already installed image without a prepare call.
   * A newly written candidate always keeps its native link constraint. */
  if (h2_bk_fixed_layout_active()) {
    int rc = h2_bk_image_publish_native_footer(id);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_bk_fixed_select(id);
    if (rc == H2_PAL_OK) {
      h2_bk_image_selected(id);
      s_prepared_partition = 0u;
    }
    return rc;
  }
  update_part_flag =
      id == H2_BK_PRIMARY_PARTITION_ID ? UPDATE_A_PART : UPDATE_B_PART;
  const int staged = h2_bk_image_prepared(id);
  const int trial = (s_prepared_partition == id &&
                     s_policy != H2_PAL_POWER_BOOT_POLICY_EXISTING) ||
                    (!s_prepared_partition && staged);
  int rc;
  if (id == H2_BK_APP_PARTITION_ID && staged) {
    /* Keep the SDK's native commit for a freshly written legacy B image. */
    (void)bk_wdt_feed();
    rc = bk_ota_update_partition_flag(BK_OK) == BK_OK ? H2_PAL_OK
                                                      : H2_PAL_ERR_IO;
    (void)bk_wdt_feed();
    printf("H2_BK_OTA_WRITER stage=commit rc=%d\r\n", rc);
  } else {
    rc = write_execution_flags((uint8_t)(id - 1u), (uint8_t)(id - 1u),
                               trial ? 1u : (uint8_t)(id + 2u));
  }
  if (rc == H2_PAL_OK) {
    h2_bk_image_selected(id);
    s_prepared_partition = 0u;
  }
  return rc;
}

int h2_bk_platform_boot_startup(void) {
  if (h2_bk_fixed_layout_active()) return H2_PAL_OK;
  if (h2_bk_fixed_current_slot() != EXEC_B_PART) return H2_PAL_OK;
  /* Legacy SDK A/B: every B boot is a trial until that boot confirms. Arm A
   * as the reset target while retaining B as the attempted image; seen from
   * A, A/B/confirmed-A is the evidence of an unconfirmed attempt. */
  return write_execution_flags(EXEX_A_PART, EXEC_B_PART, CONFIRM_EXEC_A);
}

static h2_pal_result_t power_confirm(
    void* user, const h2_pal_power_native_evidence_t* evidence,
    h2_pal_power_image_confirmation_t* out) {
  (void)user;
  if (!out || (evidence && (evidence->format || evidence->size)))
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (h2_bk_image_write_active()) return H2_PAL_ERR_BUSY;
  const uint32_t id = h2_bk_fixed_current_slot() == 0u
                          ? H2_BK_PRIMARY_PARTITION_ID
                          : H2_BK_APP_PARTITION_ID;
  h2_pal_power_image_state_t state;
  int rc = power_get_image_state(NULL, id, &state);
  if (rc != H2_PAL_OK) return rc;
  if (h2_bk_fixed_layout_active()) {
    /* This is a native linkage fact from the board's active partition table,
     * not an application/package role supplied by a caller. */
    const h2_fixed_layout_t* layout = h2_bk_fixed_layout();
    rc = layout->app_table ? h2_bk_fixed_confirm_app()
                           : h2_bk_fixed_confirm_loader();
  } else {
#if CONFIG_OTA_POSITION_INDEPENDENT_AB
    bk_ota_double_check_for_execution();
#else
    bk_ota_confirm_update_partition(
        id == H2_BK_PRIMARY_PARTITION_ID ? CONFIRM_EXEC_A : CONFIRM_EXEC_B);
#endif
    /* Confirming A is the SDK operation alone: rewriting A/A would erase the
     * A/B/confirmed-A evidence of an unconfirmed B attempt. A confirmed B is
     * additionally pinned as both final and temporary image. */
    if (id == H2_BK_APP_PARTITION_ID)
      rc = write_execution_flags(EXEC_B_PART, EXEC_B_PART, CONFIRM_EXEC_B);
  }
  if (rc != H2_PAL_OK) return rc;
  out->partition_id = id;
  out->was_pending = state.state == H2_PAL_POWER_IMAGE_STATE_PENDING_VERIFY;
  h2_pal_power_image_state_t confirmed_state;
  rc = power_get_image_state(NULL, id, &confirmed_state);
  if (rc != H2_PAL_OK) return rc;
  out->flags =
      confirmed_state.flags & (H2_PAL_POWER_IMAGE_FLAG_RECOVERY_REQUIRED |
                               H2_PAL_POWER_IMAGE_FLAG_RECOVERY_READY |
                               H2_PAL_POWER_IMAGE_FLAG_RECOVERY_PUBLISHED);
  return H2_PAL_OK;
}

static h2_pal_result_t power_reboot(void* user, uint32_t reason) {
  (void)user;
  (void)reason;
  h2_bk_platform_sd_fs_release();
  bk_wdt_force_reboot();
  return H2_PAL_OK;
}

static h2_pal_result_t power_feed_watchdog(void* user) {
  (void)user;
  /* Preserve the configured watchdog only; SDK force_feed also enables the
   * otherwise unserviced AON watchdog. */
  return bk_wdt_feed() == BK_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

const h2_pal_power_api_t* h2_bk_platform_power_api(void) {
  static const h2_pal_power_vtable_t vtable = {
      .get_capabilities = power_get_capabilities,
      .list_boot_partitions = power_list_boot_partitions,
      .get_running_boot_partition = power_get_running_boot_partition,
      .get_next_boot_partition = power_get_next_boot_partition,
      .set_next_boot_partition = power_set_next_boot_partition,
      .prepare_next_boot_partition = power_prepare,
      .get_image_state = power_get_image_state,
      .confirm_running_image = power_confirm,
      .reboot = power_reboot,
      .feed_watchdog = power_feed_watchdog,
  };
  static const h2_pal_power_api_t api = {.vtable = &vtable};
  return &api;
}
