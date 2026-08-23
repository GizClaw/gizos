#include "h2_bk_h2loader.h"
#include "h2_bk_h2loader_internal.h"

#include "bk_private/bk_ota_private.h"
#include "components/system.h"
#include "h2/pal/core/h2_pal_errors.h"
#include "modules/ota.h"

#include <stdio.h>
#include <string.h>

extern part_flag update_part_flag;

static h2_pal_result_t commit_next_partition(part_flag target) {
    int rc;

    update_part_flag = target;
    if (target == UPDATE_B_PART) {
        rc = h2_bk_h2loader_commit_staged_app_boot();
        if (rc == H2_PAL_OK) {
            return H2_PAL_OK;
        }
        if (rc != H2_PAL_ERR_INVALID_STATE) {
            return rc;
        }
    }
    return h2_bk_h2loader_select_confirmed_boot_partition(
        target == UPDATE_A_PART ?
            H2_BK_H2LOADER_PRIMARY_PARTITION_ID :
            H2_BK_H2LOADER_APP_PARTITION_ID);
}

static h2_pal_result_t power_get_capabilities(void *user, h2_pal_power_capabilities_t *out_capabilities) {
    (void)user;
    if (out_capabilities == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_capabilities->flags =
        H2_PAL_POWER_CAPABILITY_REBOOT |
        H2_PAL_POWER_CAPABILITY_BOOT_PARTITIONS |
        H2_PAL_POWER_CAPABILITY_SET_NEXT_BOOT_PARTITION;
    return H2_PAL_OK;
}

static void fill_partition(h2_pal_power_boot_partition_t *out_partition, uint32_t id, const char *name, uint32_t flags) {
    memset(out_partition, 0, sizeof(*out_partition));
    out_partition->id = id;
    out_partition->flags = flags;
    (void)snprintf(out_partition->name, sizeof(out_partition->name), "%s", name);
}

static h2_pal_result_t power_list_boot_partitions(
    void *user,
    h2_pal_power_boot_partition_cb_t cb,
    void *cb_user) {
    h2_pal_power_boot_partition_t partition;
    h2_pal_result_t rc;

    (void)user;
    if (cb == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    fill_partition(&partition,
        H2_BK_H2LOADER_PRIMARY_PARTITION_ID,
        "primary_loader",
        H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE | H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY);
    rc = cb(cb_user, &partition);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    fill_partition(&partition,
        H2_BK_H2LOADER_APP_PARTITION_ID,
        "s_app",
        H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE | H2_PAL_POWER_BOOT_PARTITION_FLAG_APP);
    return cb(cb_user, &partition);
}

static h2_pal_result_t power_get_running_boot_partition(void *user, h2_pal_power_boot_partition_t *out_partition) {
    uint8_t current;

    (void)user;
    if (out_partition == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    current = bk_ota_get_current_partition();
    if (current == 0u) {
        fill_partition(out_partition,
            H2_BK_H2LOADER_PRIMARY_PARTITION_ID,
            "primary_loader",
            H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
                H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING |
                H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY);
    } else {
        fill_partition(out_partition,
            H2_BK_H2LOADER_APP_PARTITION_ID,
            "s_app",
            H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
                H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING |
                H2_PAL_POWER_BOOT_PARTITION_FLAG_APP);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t power_set_next_boot_partition(void *user, uint32_t partition_id) {
    (void)user;
    switch (partition_id) {
    case H2_BK_H2LOADER_PRIMARY_PARTITION_ID:
        return commit_next_partition(UPDATE_A_PART);
    case H2_BK_H2LOADER_APP_PARTITION_ID:
        return commit_next_partition(UPDATE_B_PART);
    default:
        return H2_PAL_ERR_INVALID_ARG;
    }
}

static h2_pal_result_t power_reboot(void *user, uint32_t reason) {
    (void)user;
    (void)reason;
    bk_reboot();
    return H2_PAL_OK;
}

const h2_pal_power_api_t *h2_bk_h2loader_power_api(void) {
    static const h2_pal_power_vtable_t vtable = {
        .get_capabilities = power_get_capabilities,
        .list_boot_partitions = power_list_boot_partitions,
        .get_running_boot_partition = power_get_running_boot_partition,
        .set_next_boot_partition = power_set_next_boot_partition,
        .reboot = power_reboot,
    };
    static const h2_pal_power_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
