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
    uint64_t states;
    const char *board;
    const char *target;
    const char *chip;

    if (status == NULL || out == NULL || out_len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (h2_loader_states_pack(status, &states) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    board = default_if_empty(status->board, "unknown");
    target = default_if_empty(status->target, "unknown");
    chip = default_if_empty(status->chip, target);
    len = snprintf(out,
        out_len,
        "H2_LOADER_STATUS board=%s target=%s chip=%s capabilities=0x%08lx command_availability=0x%08lx states=0x%016llx "
        "active_name=%s active_version=%s active_checksum=%s last=%d "
        "installed_version=%s installed_checksum=%s "
        "staged_version=%s staged_checksum=%s staged_bytes=%llu "
        "running_partition=%lu next_partition=%lu canonical_partition=%lu trial_partition=%lu "
        "upgrade_last=%ld upgrade_step=%s upgrade_package_sha256=%s candidate_board=%s candidate_target=%s "
        "candidate_version=%s candidate_bytes=%llu candidate_sha256=%s",
        board,
        target,
        chip,
        (unsigned long)status->capabilities,
        (unsigned long)status->command_availability,
        (unsigned long long)states,
        default_if_empty(status->active_name, "unknown"),
        default_if_empty(status->active_version, ""),
        default_if_empty(status->active_checksum, ""),
        status->last_result,
        status->installed.version,
        status->installed.checksum,
        status->staged.version,
        status->staged.checksum,
        (unsigned long long)status->staged.size,
        (unsigned long)status->running_partition_id,
        (unsigned long)status->next_partition_id,
        (unsigned long)status->loader_upgrade.canonical_partition,
        (unsigned long)status->loader_upgrade.trial_partition,
        (long)status->loader_upgrade.last_result,
        status->loader_upgrade_step,
        status->loader_upgrade.package_sha256,
        status->loader_upgrade.candidate.board,
        status->loader_upgrade.candidate.target,
        status->loader_upgrade.candidate.version,
        (unsigned long long)status->loader_upgrade.candidate.image_size,
        status->loader_upgrade.candidate.image_sha256);
    if (len < 0 || (size_t)len >= out_len) {
        return H2_PAL_ERR_NO_SPACE;
    }
    return H2_PAL_OK;
}
