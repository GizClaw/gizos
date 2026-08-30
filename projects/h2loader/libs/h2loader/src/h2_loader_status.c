#include "h2_loader_status.h"

#include <stdio.h>
#include <string.h>

static const char *default_if_empty(const char *value, const char *fallback) {
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static void copy_text(char *dst, size_t dst_len, const char *src) {
    size_t len;

    if (dst == NULL || dst_len == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    len = strlen(src);
    if (len >= dst_len) {
        len = dst_len - 1u;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static uint32_t mfg_passed_count(const h2_loader_mfg_summary_t *summary) {
    uint32_t passed = 0u;
    if (summary != NULL) {
        for (uint32_t i = 0u; i < summary->total; ++i) {
            passed += summary->step_status[i] == H2_LOADER_MFG_STEP_PASSED;
        }
    }
    return passed;
}

const char *h2_loader_mfg_state_name(const h2_loader_mfg_summary_t *summary) {
    if (summary == NULL ||
        h2_loader_mfg_summary_validate(summary) != H2_PAL_OK) {
        return "invalid";
    }
    if (summary->total == 0u) {
        return "disabled";
    }
    return mfg_passed_count(summary) == summary->total ? "passed" : "partial";
}

int h2_loader_mfg_summary_validate(const h2_loader_mfg_summary_t *summary) {
    if (summary == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (summary->total != 0u && summary->total != H2_LOADER_MFG_STEP_TOTAL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0u; i < H2_LOADER_MFG_STEP_TOTAL; ++i) {
        if (summary->step_status[i] > H2_LOADER_MFG_STEP_FAILED ||
            (summary->total == 0u &&
             summary->step_status[i] != H2_LOADER_MFG_STEP_UNTESTED)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    return H2_PAL_OK;
}

int h2_loader_mfg_summary_is_passed(
    const h2_loader_mfg_summary_t *summary,
    uint32_t required_total) {
    return summary != NULL && required_total > 0u &&
        h2_loader_mfg_summary_validate(summary) == H2_PAL_OK &&
        summary->total == required_total &&
        mfg_passed_count(summary) == required_total;
}

int h2_loader_status_mfg_handoff_pending(
    const h2_loader_status_t *status,
    int staged_loader,
    int *out_pending) {
    int pending = 0;

    if (status == NULL || out_pending == NULL ||
        (staged_loader != 0 && staged_loader != 1)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!staged_loader &&
        status->boot_intent == H2_LOADER_BOOT_INTENT_AUTO) {
        if (status->install_state ==
                H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED ||
            status->install_state == H2_LOADER_INSTALL_STATE_INSTALLING) {
            pending = 1;
        } else if (status->install_state ==
                   H2_LOADER_INSTALL_STATE_CONFIRMED) {
            pending = status->app_confirmed && status->installed.valid;
        }
    }
    *out_pending = pending ? 1 : 0;
    return H2_PAL_OK;
}

int h2_loader_status_set_mfg(
    h2_loader_status_t *status,
    const h2_loader_mfg_summary_t *summary) {
    int rc;

    if (status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_loader_mfg_summary_validate(summary);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    status->mfg = *summary;
    return H2_PAL_OK;
}

int h2_loader_status_set_active(
    h2_loader_status_t *status,
    h2_loader_active_role_t role,
    const char *name,
    const char *version,
    const char *checksum) {
    if (status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (role != H2_LOADER_ACTIVE_ROLE_H2LOADER &&
        role != H2_LOADER_ACTIVE_ROLE_APP) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    status->active_role = role;
    copy_text(status->active_name, sizeof(status->active_name), name);
    copy_text(status->active_version, sizeof(status->active_version), version);
    copy_text(status->active_checksum, sizeof(status->active_checksum), checksum);
    return H2_PAL_OK;
}

int h2_loader_status_set_device(
    h2_loader_status_t *status,
    const char *board,
    const char *target,
    const char *chip) {
    if (status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    copy_text(status->board, sizeof(status->board), board);
    copy_text(status->target, sizeof(status->target), target);
    copy_text(status->chip, sizeof(status->chip), chip);
    return H2_PAL_OK;
}

int h2_loader_status_format(
    const h2_loader_status_t *status,
    char *out,
    size_t out_len) {
    int len;
    const char *board;
    const char *target;
    const char *chip;
    const char *active_role;
    char mfg_steps[H2_LOADER_MFG_STEP_TOTAL + 1u];

    if (status == NULL || out == NULL || out_len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (h2_loader_mfg_summary_validate(&status->mfg) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    board = default_if_empty(status->board, "unknown");
    target = default_if_empty(status->target, "unknown");
    chip = default_if_empty(status->chip, target);
    active_role = status->active_role == H2_LOADER_ACTIVE_ROLE_APP ? "app" :
        status->active_role == H2_LOADER_ACTIVE_ROLE_H2LOADER ? "loader" :
        "unknown";
    for (size_t index = 0u; index < H2_LOADER_MFG_STEP_TOTAL; ++index) {
        mfg_steps[index] = (char)('0' + status->mfg.step_status[index]);
    }
    mfg_steps[H2_LOADER_MFG_STEP_TOTAL] = '\0';
#define VALUE_OR_DASH(value) ((value)[0] != '\0' ? (value) : "-")
#define ROLE_NAME(value) \
    ((value) == H2_LOADER_IMAGE_ROLE_APP ? "app" : \
     (value) == H2_LOADER_IMAGE_ROLE_H2LOADER ? "loader" : "unknown")
    len = snprintf(out,
        out_len,
        "H2_LOADER_STATUS board=%s target=%s chip=%s capabilities=0x%08lx command_availability=0x%08lx "
        "active_role=%s active_version=%s active_checksum=%s running_partition=%lu next_partition=%lu boot_intent=%s "
        "stage_valid=%u stage_package_checksum=%s stage_package_size=%llu stage_image_checksum=%s stage_image_size=%llu stage_role=%s stage_version=%s stage_board=%s stage_target=%s "
        "partition_1_valid=%u partition_1_package_checksum=%s partition_1_package_size=%llu partition_1_image_checksum=%s partition_1_image_size=%llu partition_1_role=%s partition_1_version=%s partition_1_board=%s partition_1_target=%s "
        "partition_2_valid=%u partition_2_package_checksum=%s partition_2_package_size=%llu partition_2_image_checksum=%s partition_2_image_size=%llu partition_2_role=%s partition_2_version=%s partition_2_board=%s partition_2_target=%s "
        "last_result=%d mfg_mode=%u mfg_steps=%s",
        board,
        target,
        chip,
        (unsigned long)status->capabilities,
        (unsigned long)status->command_availability,
        active_role,
        VALUE_OR_DASH(status->active_version),
        VALUE_OR_DASH(status->active_checksum),
        (unsigned long)status->running_partition_id,
        (unsigned long)status->next_partition_id,
        h2_loader_boot_intent_name(status->boot_intent),
        (unsigned)(status->stage.valid != 0),
        VALUE_OR_DASH(status->stage.package_checksum),
        (unsigned long long)status->stage.package_size,
        VALUE_OR_DASH(status->stage.image_checksum),
        (unsigned long long)status->stage.image_size,
        ROLE_NAME(status->stage.role),
        VALUE_OR_DASH(status->stage.version),
        VALUE_OR_DASH(status->stage.board),
        VALUE_OR_DASH(status->stage.target),
        (unsigned)(status->partition_1.valid != 0),
        VALUE_OR_DASH(status->partition_1.package_checksum),
        (unsigned long long)status->partition_1.package_size,
        VALUE_OR_DASH(status->partition_1.image_checksum),
        (unsigned long long)status->partition_1.image_size,
        ROLE_NAME(status->partition_1.role),
        VALUE_OR_DASH(status->partition_1.version),
        VALUE_OR_DASH(status->partition_1.board),
        VALUE_OR_DASH(status->partition_1.target),
        (unsigned)(status->partition_2.valid != 0),
        VALUE_OR_DASH(status->partition_2.package_checksum),
        (unsigned long long)status->partition_2.package_size,
        VALUE_OR_DASH(status->partition_2.image_checksum),
        (unsigned long long)status->partition_2.image_size,
        ROLE_NAME(status->partition_2.role),
        VALUE_OR_DASH(status->partition_2.version),
        VALUE_OR_DASH(status->partition_2.board),
        VALUE_OR_DASH(status->partition_2.target),
        status->last_result,
        (unsigned)(status->mfg.total == 0u
            ? H2_LOADER_MFG_MODE_DISABLED
            : H2_LOADER_MFG_MODE_ENABLED),
        mfg_steps);
#undef ROLE_NAME
#undef VALUE_OR_DASH
    if (len < 0 || (size_t)len >= out_len) {
        return H2_PAL_ERR_NO_SPACE;
    }
    return H2_PAL_OK;
}
