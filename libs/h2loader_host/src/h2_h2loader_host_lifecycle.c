#include "h2_h2loader_host_lifecycle.h"

#include <string.h>

static int metadata_equal(const h2_h2loader_host_metadata_t* left,
                          const h2_h2loader_host_metadata_t* right) {
  return left->valid == right->valid && left->role == right->role &&
         left->package_size == right->package_size &&
         left->image_size == right->image_size &&
         strcmp(left->package_checksum, right->package_checksum) == 0 &&
         strcmp(left->image_checksum, right->image_checksum) == 0 &&
         strcmp(left->version, right->version) == 0 &&
         strcmp(left->board, right->board) == 0 &&
         strcmp(left->target, right->target) == 0;
}

static int metadata_matches_stage(const h2_h2loader_host_metadata_t* partition,
                                  const h2_h2loader_host_metadata_t* stage) {
  return partition->valid && stage->valid && partition->role == stage->role &&
         partition->package_size == stage->package_size &&
         partition->image_size == stage->image_size &&
         strcmp(partition->package_checksum, stage->package_checksum) == 0 &&
         strcmp(partition->image_checksum, stage->image_checksum) == 0 &&
         strcmp(partition->version, stage->version) == 0 &&
         strcmp(partition->board, stage->board) == 0 &&
         strcmp(partition->target, stage->target) == 0;
}

static int metadata_image_equal(const h2_h2loader_host_metadata_t* left,
                                const h2_h2loader_host_metadata_t* right) {
  return left->valid && right->valid && left->role == right->role &&
         left->image_size == right->image_size &&
         strcmp(left->image_checksum, right->image_checksum) == 0 &&
         strcmp(left->version, right->version) == 0 &&
         strcmp(left->board, right->board) == 0 &&
         strcmp(left->target, right->target) == 0;
}

static h2_pal_result_t verify_upgrade_without_stage(
    const h2_h2loader_host_status_t* before,
    const h2_h2loader_host_status_t* after) {
  const h2_h2loader_host_metadata_t* candidate = &before->partition_2;
  const int different_candidate =
      candidate->valid &&
      !metadata_image_equal(&before->partition_1, candidate);

  if (after->stage.valid ||
      after->boot_intent != H2_H2LOADER_HOST_BOOT_INTENT_AUTO ||
      after->last != H2_PAL_OK) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (!different_candidate) {
    return after->running_partition == 1u && after->next_partition == 1u &&
                   after->active_role == H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_STATE;
  }
  if (candidate->role == H2_H2LOADER_HOST_ACTIVE_ROLE_APP) {
    return after->running_partition == 2u && after->next_partition == 2u &&
                   after->active_role == H2_H2LOADER_HOST_ACTIVE_ROLE_APP &&
                   metadata_equal(&after->partition_2, candidate)
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_STATE;
  }
  if (candidate->role == H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER) {
    return after->running_partition == 1u && after->next_partition == 1u &&
                   after->active_role == H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER &&
                   metadata_equal(&after->partition_1, candidate) &&
                   metadata_equal(&after->partition_2, candidate)
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_STATE;
  }
  return H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t h2_h2loader_host_verify_reboot_status(
    h2_h2loader_host_command_t kind, const h2_h2loader_host_status_t* before,
    const h2_h2loader_host_status_t* after) {
  if (before == NULL || after == NULL) return H2_PAL_ERR_INVALID_ARG;
  uint32_t expected_partition;
  h2_h2loader_host_active_role_t expected_role;
  h2_h2loader_host_boot_intent_t expected_intent;
  if (kind == H2_H2LOADER_HOST_COMMAND_REBOOT_APP) {
    expected_partition = 2u;
    expected_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
    expected_intent = H2_H2LOADER_HOST_BOOT_INTENT_AUTO;
  } else if (kind == H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER) {
    expected_partition = 1u;
    expected_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
    expected_intent = H2_H2LOADER_HOST_BOOT_INTENT_LOADER;
  } else if (kind == H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE &&
             before->stage.valid &&
             before->stage.role == H2_H2LOADER_HOST_ACTIVE_ROLE_APP) {
    expected_partition = 2u;
    expected_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
    expected_intent = H2_H2LOADER_HOST_BOOT_INTENT_AUTO;
  } else if (kind == H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE &&
             before->stage.valid &&
             before->stage.role == H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER) {
    expected_partition = 1u;
    expected_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
    expected_intent = H2_H2LOADER_HOST_BOOT_INTENT_AUTO;
  } else if (kind == H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE &&
             !before->stage.valid) {
    return verify_upgrade_without_stage(before, after);
  } else {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (after->running_partition != expected_partition ||
      after->next_partition != expected_partition ||
      after->active_role != expected_role ||
      after->boot_intent != expected_intent ||
      (kind == H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE &&
       after->last != H2_PAL_OK)) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (kind == H2_H2LOADER_HOST_COMMAND_REBOOT_APP ||
      kind == H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER) {
    /* last_result describes installation/rollback, not this reboot.
     * set_next_and_reboot preserves it; an old failure must not force
     * 120 reconnects after the requested role is already responsive. */
    return metadata_equal(&before->stage, &after->stage)
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_STATE;
  }
  if (after->stage.valid) return H2_PAL_ERR_INVALID_STATE;
  if (expected_role == H2_H2LOADER_HOST_ACTIVE_ROLE_APP) {
    return metadata_matches_stage(&after->partition_2, &before->stage)
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_STATE;
  }
  return metadata_matches_stage(&after->partition_1, &before->stage) &&
                 metadata_matches_stage(&after->partition_2, &before->stage)
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}

int h2_h2loader_host_app_awaits_confirmation(
    const h2_h2loader_host_status_t* before,
    const h2_h2loader_host_status_t* after) {
  if (before == NULL || after == NULL) return 0;
  return before->stage.valid &&
         before->stage.role == H2_H2LOADER_HOST_ACTIVE_ROLE_APP &&
         after->stage.valid && metadata_equal(&after->stage, &before->stage) &&
         after->active_role == H2_H2LOADER_HOST_ACTIVE_ROLE_APP &&
         after->running_partition == 2u && after->next_partition == 2u &&
         after->boot_intent == H2_H2LOADER_HOST_BOOT_INTENT_AUTO &&
         after->last == H2_PAL_OK &&
         strcmp(after->device_uid, before->device_uid) == 0 &&
         strcmp(after->board, before->board) == 0 &&
         strcmp(after->target, before->target) == 0 &&
         metadata_equal(&after->partition_1, &before->partition_1) &&
         metadata_matches_stage(&after->partition_2, &before->stage);
}
