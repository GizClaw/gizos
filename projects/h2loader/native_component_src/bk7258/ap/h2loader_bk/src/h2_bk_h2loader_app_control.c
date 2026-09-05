#include "h2_bk_h2loader.h"
#include "h2_bk_h2loader_internal.h"

#include "h2_loader_status.h"

typedef struct h2_bk_app_power {
  const h2_pal_pref_api_t *pref;
  int returning_to_loader;
} h2_bk_app_power_t;

static h2_bk_app_power_t s_app_power;

static h2_pal_result_t app_power_get_capabilities(
    void *user, h2_pal_power_capabilities_t *out_capabilities) {
  (void)user;
  return h2_pal_power_get_capabilities(h2_bk_h2loader_power_api(),
                                       out_capabilities);
}

static h2_pal_result_t app_power_list_boot_partitions(
    void *user, h2_pal_power_boot_partition_cb_t cb, void *cb_user) {
  (void)user;
  return h2_pal_power_list_boot_partitions(h2_bk_h2loader_power_api(), cb,
                                           cb_user);
}

static h2_pal_result_t app_power_get_running_boot_partition(
    void *user, h2_pal_power_boot_partition_t *out_partition) {
  (void)user;
  return h2_pal_power_get_running_boot_partition(h2_bk_h2loader_power_api(),
                                                  out_partition);
}

static h2_pal_result_t app_power_set_next_boot_partition(
    void *user, uint32_t partition_id) {
  h2_bk_app_power_t *power = user;
  h2_pal_result_t rc = h2_pal_power_set_next_boot_partition(
      h2_bk_h2loader_power_api(), partition_id);
  if (rc == H2_PAL_OK) {
    power->returning_to_loader =
        partition_id == H2_BK_H2LOADER_PRIMARY_PARTITION_ID;
  }
  return rc;
}

static h2_pal_result_t app_power_reboot(void *user, uint32_t reason) {
  h2_bk_app_power_t *power = user;
  if (!power->returning_to_loader) {
    h2_pal_result_t rc = h2_bk_h2loader_prepare_pending_app_restart();
    if (rc != H2_PAL_OK) {
      return rc;
    }
  }
  power->returning_to_loader = 0;
  h2_bk_h2loader_release_sd_storage();
  return h2_pal_power_reboot(h2_bk_h2loader_power_api(), reason);
}

const h2_pal_power_api_t *h2_bk_h2loader_app_power_api(
    const h2_pal_pref_api_t *pref) {
  static const h2_pal_power_vtable_t vtable = {
      .get_capabilities = app_power_get_capabilities,
      .list_boot_partitions = app_power_list_boot_partitions,
      .get_running_boot_partition = app_power_get_running_boot_partition,
      .set_next_boot_partition = app_power_set_next_boot_partition,
      .reboot = app_power_reboot,
  };
  static const h2_pal_power_api_t api = {
      .user = &s_app_power,
      .vtable = &vtable,
  };
  if (pref == NULL) {
    return NULL;
  }
  if (s_app_power.pref == NULL) {
    s_app_power.pref = pref;
  } else if (s_app_power.pref != pref) {
    return NULL;
  }
  return &api;
}

int h2_bk_h2loader_reboot_to_loader(void) {
  const h2_pal_power_api_t *power = h2_bk_h2loader_power_api();
  int rc = h2_pal_power_set_next_boot_partition(
      power, H2_BK_H2LOADER_PRIMARY_PARTITION_ID);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  h2_bk_h2loader_release_sd_storage();
  return h2_pal_power_reboot(power, 0u);
}
