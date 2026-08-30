#include "h2_loader_boot.h"
#include "h2_loader_stage.h"
#include "h2_loader_status.h"

#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#define H2_LOADER_REBOOT_REASON_DEFAULT 0u
#define H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED (UINT32_C(1) << 30)

static const char *default_if_empty(const char *value, const char *fallback) {
  return value != NULL && value[0] != '\0' ? value : fallback;
}

static void copy_text(char *out, size_t capacity, const char *value) {
  if (out == NULL || capacity == 0u)
    return;
  (void)snprintf(out, capacity, "%s", value != NULL ? value : "");
}

static int atomic_load(const h2_loader_atomic_flag_t *value) {
#if defined(_MSC_VER)
  return (int)_InterlockedCompareExchange((volatile long *)value, 0, 0);
#else
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static void atomic_store(h2_loader_atomic_flag_t *value, int stored) {
#if defined(_MSC_VER)
  (void)_InterlockedExchange((volatile long *)value, (long)stored);
#else
  __atomic_store_n(value, stored, __ATOMIC_RELEASE);
#endif
}

static void atomic_update_flags(h2_loader_atomic_flag_t *value, uint32_t flags,
                                bool available) {
  int expected = atomic_load(value);
  for (;;) {
    uint32_t base = (uint32_t)expected;
    if ((base & H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED) == 0u) {
      base = H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED |
             H2_LOADER_COMMAND_AVAILABILITY_ALL;
    }
    uint32_t next = available ? base | flags : base & ~flags;
    next |= H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED;
#if defined(_MSC_VER)
    int observed = (int)_InterlockedCompareExchange((volatile long *)value,
                                                    (long)next, (long)expected);
    if (observed == expected)
      return;
    expected = observed;
#else
    if (__atomic_compare_exchange_n(value, &expected, (int)next, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      return;
    }
#endif
  }
}

static uint32_t load_availability(const h2_loader_atomic_flag_t *value) {
  uint32_t stored = (uint32_t)atomic_load(value);
  return (stored & H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED) != 0u
             ? stored & H2_LOADER_COMMAND_AVAILABILITY_ALL
             : H2_LOADER_COMMAND_AVAILABILITY_ALL;
}

static int mfg_gate_satisfied(const h2_loader_t *loader) {
  return loader != NULL &&
         (atomic_load(&loader->mfg_gate_bypass) != 0 ||
          loader->config.mfg_required_total == 0u ||
          h2_loader_mfg_summary_is_passed(&loader->status.mfg,
                                          loader->config.mfg_required_total));
}

int h2_loader_set_mfg_gate_bypass(h2_loader_t *loader, int enabled) {
  if (loader == NULL || (enabled != 0 && enabled != 1)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  atomic_store(&loader->mfg_gate_bypass, enabled);
  return H2_PAL_OK;
}

int h2_loader_set_implemented_commands(h2_loader_t *loader, uint32_t commands) {
  if (loader == NULL ||
      (commands & ~H2_LOADER_COMMAND_AVAILABILITY_ALL) != 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  atomic_store(&loader->implemented_commands,
               (int)(H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED | commands));
  return H2_PAL_OK;
}

int h2_loader_set_command_availability(h2_loader_t *loader, uint32_t flags,
                                       bool available) {
  if (loader == NULL || flags == 0u ||
      (flags & ~H2_LOADER_COMMAND_AVAILABILITY_ALL) != 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  atomic_update_flags(&loader->command_availability, flags, available);
  return H2_PAL_OK;
}

uint32_t h2_loader_get_command_availability(const h2_loader_t *loader,
                                            const h2_loader_status_t *status) {
  uint32_t available;
  if (loader == NULL || status == NULL)
    return 0u;
  available = load_availability(&loader->implemented_commands) &
              load_availability(&loader->command_availability);
  if ((status->capabilities & H2_LOADER_CAPABILITY_WIFI) == 0u) {
    available &= ~(H2_LOADER_COMMAND_AVAILABLE_WIFI_SCAN |
                   H2_LOADER_COMMAND_AVAILABLE_WIFI_CONNECT |
                   H2_LOADER_COMMAND_AVAILABLE_WIFI_DISCONNECT |
                   H2_LOADER_COMMAND_AVAILABLE_STAGE_URL);
  }
  return available;
}

const char *h2_loader_boot_intent_name(h2_loader_boot_intent_t intent) {
  switch (intent) {
  case H2_LOADER_BOOT_INTENT_LOADER:
    return "loader";
  case H2_LOADER_BOOT_INTENT_AUTO:
    return "auto";
  default:
    return "unknown";
  }
}

static int pref_open(const h2_pal_pref_api_t *pref,
                     h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t **out_ns) {
  if (pref == NULL || out_ns == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_ns = NULL;
  return h2_pal_pref_open(pref, H2_LOADER_PREF_NAMESPACE, mode, out_ns);
}

static int pref_set_u32(const h2_pal_pref_api_t *pref, const char *key,
                        uint32_t value) {
  h2_pal_pref_namespace_t *ns = NULL;
  int rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  int close_rc;
  if (rc != H2_PAL_OK)
    return rc;
  if (ns == NULL || ns->set_u32 == NULL || ns->commit == NULL) {
    rc = H2_PAL_ERR_UNSUPPORTED;
  } else {
    rc = ns->set_u32(ns, key, value);
    if (rc == H2_PAL_OK)
      rc = ns->commit(ns);
  }
  close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
  return rc == H2_PAL_OK ? close_rc : rc;
}

static int pref_set_i32(const h2_pal_pref_api_t *pref, const char *key,
                        int32_t value) {
  h2_pal_pref_namespace_t *ns = NULL;
  int rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  int close_rc;
  if (rc != H2_PAL_OK)
    return rc;
  if (ns == NULL || ns->set_i32 == NULL || ns->commit == NULL) {
    rc = H2_PAL_ERR_UNSUPPORTED;
  } else {
    rc = ns->set_i32(ns, key, value);
    if (rc == H2_PAL_OK)
      rc = ns->commit(ns);
  }
  close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
  return rc == H2_PAL_OK ? close_rc : rc;
}

static int read_control_fields(const h2_pal_pref_api_t *pref,
                               h2_loader_status_t *status) {
  h2_pal_pref_namespace_t *ns = NULL;
  uint32_t intent = H2_LOADER_BOOT_INTENT_LOADER;
  int32_t result = H2_PAL_OK;
  int rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  int close_rc;
  if (rc == H2_PAL_ERR_NOT_FOUND)
    return H2_PAL_OK;
  if (rc != H2_PAL_OK)
    return rc;
  if (ns == NULL) {
    rc = H2_PAL_ERR_UNSUPPORTED;
  } else {
    if (ns->get_u32 != NULL) {
      int get_rc = ns->get_u32(ns, "boot_intent", &intent);
      if (get_rc != H2_PAL_OK && get_rc != H2_PAL_ERR_NOT_FOUND)
        rc = get_rc;
    }
    if (rc == H2_PAL_OK && ns->get_i32 != NULL) {
      int get_rc = ns->get_i32(ns, "last_result", &result);
      if (get_rc != H2_PAL_OK && get_rc != H2_PAL_ERR_NOT_FOUND)
        rc = get_rc;
    }
  }
  close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
  if (rc != H2_PAL_OK)
    return rc;
  if (close_rc != H2_PAL_OK)
    return close_rc;
  if (intent != H2_LOADER_BOOT_INTENT_LOADER &&
      intent != H2_LOADER_BOOT_INTENT_AUTO) {
    return H2_PAL_ERR_FORMAT;
  }
  status->boot_intent = (h2_loader_boot_intent_t)intent;
  status->last_result = result;
  return H2_PAL_OK;
}

int h2_loader_read_pref_status(const h2_pal_pref_api_t *pref,
                               const h2_pal_mem_api_t *allocator,
                               h2_loader_status_t *out_status) {
  int present = 0;
  int rc;
  if (pref == NULL || allocator == NULL || out_status == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_status, 0, sizeof(*out_status));
  out_status->boot_intent = H2_LOADER_BOOT_INTENT_LOADER;
  rc = read_control_fields(pref, out_status);
  if (rc != H2_PAL_OK)
    return rc;
#define READ_SLOT(slot_name, field)                                            \
  do {                                                                         \
    rc = h2_loader_metadata_read(pref, allocator, (slot_name),                 \
                                 &out_status->field, &present);                \
    if (rc != H2_PAL_OK)                                                       \
      return rc;                                                               \
  } while (0)
  READ_SLOT(H2_LOADER_METADATA_SLOT_STAGE, stage);
  READ_SLOT(H2_LOADER_METADATA_SLOT_PARTITION_1, partition_1);
  READ_SLOT(H2_LOADER_METADATA_SLOT_PARTITION_2, partition_2);
#undef READ_SLOT
  rc = h2_loader_mfg_read(pref, allocator, &out_status->mfg, &present);
  if (rc != H2_PAL_OK)
    return rc;
  if (!present)
    memset(&out_status->mfg, 0, sizeof(out_status->mfg));
  return H2_PAL_OK;
}

static void status_from_config(const h2_loader_config_t *config,
                               h2_loader_status_t *status) {
  h2_loader_active_role_t role = H2_LOADER_ACTIVE_ROLE_UNKNOWN;
  if (config->active_identity.role == H2_LOADER_IMAGE_ROLE_H2LOADER) {
    role = H2_LOADER_ACTIVE_ROLE_H2LOADER;
  } else if (config->active_identity.role == H2_LOADER_IMAGE_ROLE_APP) {
    role = H2_LOADER_ACTIVE_ROLE_APP;
  }
  (void)h2_loader_status_set_device(status, config->board, config->target,
                                    config->chip);
  if (role != H2_LOADER_ACTIVE_ROLE_UNKNOWN) {
    (void)h2_loader_status_set_active(
        status, role, role == H2_LOADER_ACTIVE_ROLE_APP ? "app" : "loader",
        config->active_identity.version, config->active_identity.image_sha256);
    status->active_image_size = config->active_identity.image_size;
  }
  status->capabilities = config->hardware_capabilities;
}

static int refresh_partitions(h2_loader_t *loader, h2_loader_status_t *status) {
  h2_pal_power_boot_partition_t partition;
  int rc =
      h2_pal_power_get_running_boot_partition(loader->config.power, &partition);
  if (rc != H2_PAL_OK)
    return rc;
  status->running_partition_id = partition.id;
  rc = h2_pal_power_get_next_boot_partition(loader->config.power, &partition);
  if (rc == H2_PAL_ERR_UNSUPPORTED) {
    status->next_partition_id = status->running_partition_id;
    return H2_PAL_OK;
  }
  if (rc != H2_PAL_OK)
    return rc;
  status->next_partition_id = partition.id;
  return H2_PAL_OK;
}

int h2_loader_read_status(h2_loader_t *loader, h2_loader_status_t *out_status) {
  int rc;
  if (loader == NULL || out_status == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  rc = h2_loader_read_pref_status(loader->config.pref,
                                  loader->config.package.allocator, out_status);
  if (rc != H2_PAL_OK)
    return rc;
  status_from_config(&loader->config, out_status);
  rc = refresh_partitions(loader, out_status);
  if (rc != H2_PAL_OK)
    return rc;
  out_status->command_availability =
      h2_loader_get_command_availability(loader, out_status);
  return H2_PAL_OK;
}

static int identity_valid(const h2_loader_image_identity_t *identity) {
  h2_loader_metadata_t metadata = {0};
  if (identity == NULL)
    return 0;
  metadata.valid = 1;
  metadata.role = identity->role;
  metadata.image_size = identity->image_size;
  copy_text(metadata.image_checksum, sizeof(metadata.image_checksum),
            identity->image_sha256);
  copy_text(metadata.version, sizeof(metadata.version), identity->version);
  copy_text(metadata.board, sizeof(metadata.board), identity->board);
  copy_text(metadata.target, sizeof(metadata.target), identity->target);
  return h2_loader_metadata_validate(H2_LOADER_METADATA_SLOT_PARTITION_1,
                                     &metadata) == H2_PAL_OK;
}

static void metadata_from_identity(const h2_loader_image_identity_t *identity,
                                   h2_loader_metadata_t *metadata) {
  memset(metadata, 0, sizeof(*metadata));
  metadata->valid = 1;
  metadata->role = identity->role;
  metadata->image_size = identity->image_size;
  copy_text(metadata->image_checksum, sizeof(metadata->image_checksum),
            identity->image_sha256);
  copy_text(metadata->version, sizeof(metadata->version), identity->version);
  copy_text(metadata->board, sizeof(metadata->board), identity->board);
  copy_text(metadata->target, sizeof(metadata->target), identity->target);
}

static int
metadata_matches_identity(const h2_loader_metadata_t *metadata,
                          const h2_loader_image_identity_t *identity) {
  h2_loader_metadata_t active;
  if (!identity_valid(identity))
    return 0;
  metadata_from_identity(identity, &active);
  return h2_loader_metadata_image_equal(metadata, &active);
}

static h2_loader_metadata_slot_t slot_for_partition(const h2_loader_t *loader,
                                                    uint32_t partition_id) {
  if (partition_id == loader->config.h2loader_partition_id) {
    return H2_LOADER_METADATA_SLOT_PARTITION_1;
  }
  if (partition_id == loader->config.app_partition_id) {
    return H2_LOADER_METADATA_SLOT_PARTITION_2;
  }
  return (h2_loader_metadata_slot_t)0;
}

static int seed_running_metadata(h2_loader_t *loader) {
  h2_loader_metadata_slot_t slot =
      slot_for_partition(loader, loader->status.running_partition_id);
  h2_loader_metadata_t *stored;
  h2_loader_metadata_t active;
  if (slot == 0 || !identity_valid(&loader->config.active_identity)) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  stored = slot == H2_LOADER_METADATA_SLOT_PARTITION_1
               ? &loader->status.partition_1
               : &loader->status.partition_2;
  if (metadata_matches_identity(stored, &loader->config.active_identity)) {
    return H2_PAL_OK;
  }
  metadata_from_identity(&loader->config.active_identity, &active);
  if (stored->valid && stored->image_size == active.image_size &&
      strcmp(stored->image_checksum, active.image_checksum) == 0) {
    copy_text(active.package_checksum, sizeof(active.package_checksum),
              stored->package_checksum);
    active.package_size = stored->package_size;
  }
  int rc = h2_loader_metadata_write(loader->config.pref, slot, &active);
  if (rc == H2_PAL_OK)
    *stored = active;
  return rc;
}

static void emit_event(h2_loader_t *loader, h2_loader_startup_event_t event,
                       int code) {
  if (loader->config.on_event != NULL) {
    loader->config.on_event(loader->config.event_user, event, code);
  }
}

int h2_loader_set_last_result(h2_loader_t *loader, int result) {
  if (loader == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = pref_set_i32(loader->config.pref, "last_result", result);
  if (rc == H2_PAL_OK)
    loader->status.last_result = result;
  return rc;
}

static int fail_recovery(h2_loader_t *loader, int result) {
  (void)h2_loader_set_last_result(loader, result);
  emit_event(loader, H2_LOADER_STARTUP_EVENT_RECOVERY_FAILED, result);
  return result;
}

static int remove_optional_file(const h2_pal_fs_api_t *fs, const char *path) {
  if (path == NULL || path[0] == '\0')
    return H2_PAL_OK;
  int rc = h2_pal_fs_remove(fs, path);
  return rc == H2_PAL_FS_ERR_NOT_FOUND ? H2_PAL_OK : rc;
}

int h2_loader_begin_stage(h2_loader_t *loader, const char *temporary_path,
                          const char *previous_path) {
  int rc;
  if (loader == NULL || loader->config.package.fs == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  rc = h2_loader_stage_begin(loader->config.pref);
  if (rc != H2_PAL_OK)
    return rc;
  rc = remove_optional_file(loader->config.package.fs, temporary_path);
  if (rc == H2_PAL_OK) {
    rc = remove_optional_file(loader->config.package.fs, previous_path);
  }
  if (rc == H2_PAL_OK) {
    rc = remove_optional_file(loader->config.package.fs,
                              loader->config.package.package_path);
  }
  if (rc == H2_PAL_OK)
    memset(&loader->status.stage, 0, sizeof(loader->status.stage));
  return rc;
}

int h2_loader_commit_stage(h2_loader_t *loader, uint64_t bytes,
                           const char *sha256) {
  h2_loader_metadata_t stage;
  if (loader == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_loader_stage_publish(&loader->package, loader->config.pref, bytes,
                                   sha256, &stage);
  if (rc == H2_PAL_OK)
    loader->status.stage = stage;
  return rc;
}

int h2_loader_commit_inspected_stage(
    h2_loader_t *loader, uint64_t bytes, const char *sha256,
    const h2_loader_package_inspection_t *inspection) {
  h2_loader_metadata_t stage;
  if (loader == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_loader_stage_commit_inspection(loader->config.pref, bytes, sha256,
                                             inspection, &stage);
  if (rc == H2_PAL_OK)
    loader->status.stage = stage;
  return rc;
}

int h2_loader_cancel_stage(h2_loader_t *loader) {
  if (loader == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_loader_stage_abort(loader->config.package.fs, loader->config.pref,
                                 loader->config.package.package_path);
  if (rc == H2_PAL_OK)
    memset(&loader->status.stage, 0, sizeof(loader->status.stage));
  return rc;
}

static int
stage_matches_inspection(const h2_loader_metadata_t *stage,
                         const h2_loader_package_inspection_t *inspection) {
  return stage->valid && !inspection->legacy &&
         stage->role == inspection->manifest.role &&
         stage->image_size == inspection->manifest.image_size &&
         strcmp(stage->image_checksum, inspection->manifest.image_sha256) ==
             0 &&
         strcmp(stage->version, inspection->manifest.version) == 0 &&
         strcmp(stage->board, inspection->manifest.board) == 0 &&
         strcmp(stage->target, inspection->manifest.target) == 0;
}

static int inspect_current_stage(h2_loader_t *loader,
                                 h2_loader_package_inspection_t *inspection) {
  int rc = h2_loader_package_verify_path(
      &loader->package, loader->config.package.package_path,
      loader->status.stage.package_size, loader->status.stage.package_checksum);
  if (rc == H2_PAL_OK) {
    rc = h2_loader_package_inspect_path(
        &loader->package, loader->config.package.package_path, inspection);
  }
  if (rc == H2_PAL_OK &&
      !stage_matches_inspection(&loader->status.stage, inspection)) {
    rc = H2_PAL_ERR_FORMAT;
  }
  if (rc == H2_PAL_OK) {
    inspection->staged.valid = 1;
    inspection->staged.size = loader->status.stage.package_size;
    copy_text(inspection->staged.checksum, sizeof(inspection->staged.checksum),
              loader->status.stage.package_checksum);
    copy_text(inspection->staged.version, sizeof(inspection->staged.version),
              loader->status.stage.version);
  }
  if (rc != H2_PAL_OK) {
    h2_loader_metadata_t invalid = {0};
    (void)h2_loader_metadata_write(loader->config.pref,
                                   H2_LOADER_METADATA_SLOT_STAGE, &invalid);
    loader->status.stage = invalid;
  }
  return rc;
}

static int finish_stage(const h2_pal_pref_api_t *pref,
                        const h2_pal_fs_api_t *fs, const char *package_path,
                        h2_loader_metadata_t *stage) {
  h2_loader_metadata_t invalid = {0};
  int rc =
      h2_loader_metadata_write(pref, H2_LOADER_METADATA_SLOT_STAGE, &invalid);
  if (rc != H2_PAL_OK)
    return rc;
  rc = remove_optional_file(fs, package_path);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_loader_metadata_clear(pref, H2_LOADER_METADATA_SLOT_STAGE);
  if (rc == H2_PAL_OK && stage != NULL)
    *stage = invalid;
  return rc;
}

int h2_loader_finalize_active_app(
    const h2_pal_pref_api_t *pref, const h2_pal_mem_api_t *allocator,
    const h2_pal_fs_api_t *fs, const char *package_path,
    const h2_loader_image_identity_t *active_identity,
    uint32_t running_partition_id, uint32_t app_partition_id) {
  h2_loader_metadata_t stage;
  h2_loader_metadata_t partition;
  h2_loader_metadata_t stored;
  int stage_present;
  int stored_present;
  int stage_installed;
  int rc;
  if (pref == NULL || allocator == NULL || fs == NULL || package_path == NULL ||
      active_identity == NULL ||
      active_identity->role != H2_LOADER_IMAGE_ROLE_APP ||
      app_partition_id == 0u || running_partition_id != app_partition_id ||
      !identity_valid(active_identity)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  metadata_from_identity(active_identity, &partition);
  rc = h2_loader_metadata_read(pref, allocator, H2_LOADER_METADATA_SLOT_STAGE,
                               &stage, &stage_present);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_loader_metadata_read(pref, allocator,
                               H2_LOADER_METADATA_SLOT_PARTITION_2, &stored,
                               &stored_present);
  if (rc != H2_PAL_OK)
    return rc;
  if (!stored_present || !stored.valid ||
      !h2_loader_metadata_image_equal(&stored, &partition)) {
    /* The running APP identity is authoritative even when Preference was
     * erased or contains stale P2 metadata. Seed the slot from the image, but
     * keep Stage intact because there is no durable proof that this exact
     * source package (including data-only contents) was installed. */
    return h2_loader_metadata_write(
        pref, H2_LOADER_METADATA_SLOT_PARTITION_2, &partition);
  }
  stage_installed =
      stage_present && stage.valid &&
      h2_loader_metadata_image_equal(&stage, &partition) &&
      stage.package_size == stored.package_size &&
      strcmp(stage.package_checksum, stored.package_checksum) == 0;
  if (stage_installed) {
    rc = h2_loader_metadata_from_stage(&stage, &partition);
    if (rc != H2_PAL_OK)
      return rc;
  } else {
    copy_text(partition.package_checksum, sizeof(partition.package_checksum),
              stored.package_checksum);
    partition.package_size = stored.package_size;
  }
  rc = h2_loader_metadata_write(pref, H2_LOADER_METADATA_SLOT_PARTITION_2,
                                &partition);
  if (rc != H2_PAL_OK)
    return rc;
  if (!stage_installed) {
    return H2_PAL_OK;
  }
  rc = finish_stage(pref, fs, package_path, &stage);
  return rc == H2_PAL_OK ? pref_set_i32(pref, "last_result", H2_PAL_OK) : rc;
}

static int write_partition_2(h2_loader_t *loader,
                             const h2_loader_package_inspection_t *inspection) {
  h2_loader_metadata_t invalid = {0};
  h2_loader_metadata_t installed;
  h2_loader_package_install_plan_t plan;
  h2_loader_package_install_result_t result;
  int rc = h2_loader_package_plan_install(
      &loader->package, inspection, loader->config.app_partition_id, &plan);
  if (rc != H2_PAL_OK)
    return rc;
  if (inspection->manifest.role == H2_LOADER_IMAGE_ROLE_H2LOADER) {
    plan.update_app = 1;
    plan.update_data = 0;
  }
  if (plan.update_app) {
    rc = h2_loader_metadata_write(
        loader->config.pref, H2_LOADER_METADATA_SLOT_PARTITION_2, &invalid);
    if (rc != H2_PAL_OK)
      return rc;
    loader->status.partition_2 = invalid;
  }
  emit_event(loader, H2_LOADER_STARTUP_EVENT_WRITE_PARTITION_2, H2_PAL_OK);
  rc = h2_loader_package_install_to(&loader->package, inspection,
                                    loader->config.app_partition_id, &plan,
                                    &result);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_loader_metadata_from_stage(&loader->status.stage, &installed);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_loader_metadata_write(
      loader->config.pref, H2_LOADER_METADATA_SLOT_PARTITION_2, &installed);
  if (rc == H2_PAL_OK)
    loader->status.partition_2 = installed;
  return rc;
}

static int prepare_disruptive(h2_loader_t *loader,
                              h2_loader_disruptive_action_t action) {
  return loader->config.before_disruptive != NULL
             ? loader->config.before_disruptive(loader->config.disruptive_user,
                                                action)
             : H2_PAL_OK;
}

static int set_next_and_reboot(h2_loader_t *loader, uint32_t partition_id,
                               h2_loader_boot_intent_t intent,
                               h2_loader_disruptive_action_t action,
                               h2_loader_reboot_transition_fn transition,
                               void *transition_user) {
  h2_pal_power_boot_partition_t previous_next = {0};
  h2_loader_boot_intent_t previous_intent = loader->status.boot_intent;
  int rc = h2_pal_power_get_next_boot_partition(loader->config.power,
                                                 &previous_next);
  if (rc != H2_PAL_OK)
    return rc;
  rc = prepare_disruptive(loader, action);
  if (rc != H2_PAL_OK)
    return rc;
  rc = pref_set_u32(loader->config.pref, "boot_intent", (uint32_t)intent);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_pal_power_set_next_boot_partition(loader->config.power, partition_id);
  if (rc != H2_PAL_OK) {
    (void)pref_set_u32(loader->config.pref, "boot_intent",
                       (uint32_t)previous_intent);
    return rc;
  }
  if (transition != NULL) {
    rc = transition(transition_user);
    if (rc != H2_PAL_OK) {
      (void)h2_pal_power_set_next_boot_partition(loader->config.power,
                                                  previous_next.id);
      (void)pref_set_u32(loader->config.pref, "boot_intent",
                         (uint32_t)previous_intent);
      return rc;
    }
  }
  rc = h2_pal_power_reboot(loader->config.power,
                           H2_LOADER_REBOOT_REASON_DEFAULT);
  if (rc != H2_PAL_OK) {
    (void)h2_pal_power_set_next_boot_partition(loader->config.power,
                                                previous_next.id);
    (void)pref_set_u32(loader->config.pref, "boot_intent",
                       (uint32_t)previous_intent);
    return rc;
  }
  loader->status.boot_intent = intent;
  loader->status.next_partition_id = partition_id;
  return H2_PAL_OK;
}

static int copy_partition_2_to_1(h2_loader_t *loader) {
  h2_loader_metadata_t invalid = {0};
  h2_loader_metadata_t copied;
  int rc = h2_pal_power_set_next_boot_partition(
      loader->config.power, loader->config.app_partition_id);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_loader_metadata_write(loader->config.pref,
                                H2_LOADER_METADATA_SLOT_PARTITION_1, &invalid);
  if (rc != H2_PAL_OK)
    return rc;
  loader->status.partition_1 = invalid;
  emit_event(loader, H2_LOADER_STARTUP_EVENT_COPY_PARTITION_1, H2_PAL_OK);
  rc = h2_loader_image_copy_to(
      &loader->package, &loader->config.active_identity,
      loader->config.app_partition_id, loader->config.h2loader_partition_id);
  if (rc != H2_PAL_OK)
    return rc;
  metadata_from_identity(&loader->config.active_identity, &copied);
  if (h2_loader_metadata_image_equal(&loader->status.stage, &copied)) {
    rc = h2_loader_metadata_from_stage(&loader->status.stage, &copied);
    if (rc != H2_PAL_OK)
      return rc;
    rc = h2_loader_metadata_write(loader->config.pref,
                                  H2_LOADER_METADATA_SLOT_PARTITION_2, &copied);
    if (rc != H2_PAL_OK)
      return rc;
    loader->status.partition_2 = copied;
  }
  rc = h2_loader_metadata_write(loader->config.pref,
                                H2_LOADER_METADATA_SLOT_PARTITION_1, &copied);
  if (rc != H2_PAL_OK)
    return rc;
  loader->status.partition_1 = copied;
  rc = h2_loader_set_last_result(loader, H2_PAL_OK);
  if (rc != H2_PAL_OK)
    return rc;
  return set_next_and_reboot(loader, loader->config.h2loader_partition_id,
                             H2_LOADER_BOOT_INTENT_AUTO,
                             H2_LOADER_DISRUPTIVE_REBOOT_UPGRADE, NULL, NULL);
}

static int finish_loader_stage_if_converged(h2_loader_t *loader) {
  if (!h2_loader_metadata_image_equal(&loader->status.partition_1,
                                      &loader->status.partition_2)) {
    return H2_PAL_OK;
  }
  if (!h2_loader_metadata_image_equal(&loader->status.stage,
                                      &loader->status.partition_1)) {
    return H2_PAL_OK;
  }
  h2_loader_metadata_t partition;
  int rc = h2_loader_metadata_from_stage(&loader->status.stage, &partition);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_loader_metadata_write(
      loader->config.pref, H2_LOADER_METADATA_SLOT_PARTITION_2, &partition);
  if (rc != H2_PAL_OK)
    return rc;
  loader->status.partition_2 = partition;
  rc = h2_loader_metadata_write(
      loader->config.pref, H2_LOADER_METADATA_SLOT_PARTITION_1, &partition);
  if (rc != H2_PAL_OK)
    return rc;
  loader->status.partition_1 = partition;
  rc = finish_stage(loader->config.pref, loader->config.package.fs,
                    loader->config.package.package_path, &loader->status.stage);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_loader_set_last_result(loader, H2_PAL_OK);
  if (rc != H2_PAL_OK)
    return rc;
  emit_event(loader, H2_LOADER_STARTUP_EVENT_STAGE_FINISHED, H2_PAL_OK);
  return H2_PAL_OK;
}

static int mount_file_points(h2_loader_t *loader) {
  int rc;
  if (loader->config.mount_file_point == NULL)
    return H2_PAL_OK;
  rc = loader->config.mount_file_point(loader->config.mount_user, "/dl");
  if (rc == H2_PAL_OK) {
    rc = loader->config.mount_file_point(loader->config.mount_user, "/data");
  }
  return rc;
}

int h2_loader_init(h2_loader_t *loader, const h2_loader_config_t *config) {
  int rc;
  if (loader == NULL || config == NULL || config->package.fs == NULL ||
      config->package.allocator == NULL || config->pref == NULL ||
      config->power == NULL || config->h2loader_partition_id == 0u ||
      config->app_partition_id == 0u ||
      config->h2loader_partition_id == config->app_partition_id ||
      config->hardware_capabilities == 0u ||
      (config->hardware_capabilities & ~H2_LOADER_CAPABILITIES_ALL) != 0u ||
      !identity_valid(&config->active_identity)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(loader, 0, sizeof(*loader));
  loader->config = *config;
  loader->config.package.package_path = default_if_empty(
      config->package.package_path, H2_LOADER_DEFAULT_PACKAGE_PATH);
  loader->config.package.data_root =
      default_if_empty(config->package.data_root, H2_LOADER_DEFAULT_DATA_ROOT);
  loader->config.package.installed_checksum_path = default_if_empty(
      config->package.installed_checksum_path, H2_LOADER_DEFAULT_CHECKSUM_PATH);
  loader->config.package.app_entry_path = default_if_empty(
      config->package.app_entry_path, H2_LOADER_DEFAULT_APP_ENTRY_PATH);
  if (loader->config.package.app_partition_id == 0u) {
    loader->config.package.app_partition_id = config->app_partition_id;
  }
  atomic_store(&loader->implemented_commands,
               (int)H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED);
  atomic_store(&loader->command_availability,
               (int)(H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED |
                     H2_LOADER_COMMAND_AVAILABILITY_ALL));
  rc = h2_loader_package_init(&loader->package, &loader->config.package);
  if (rc != H2_PAL_OK)
    return rc;
  return h2_loader_read_status(loader, &loader->status);
}

int h2_loader_startup(h2_loader_t *loader,
                      h2_loader_startup_action_t *out_action) {
  h2_loader_package_inspection_t inspection;
  int rc;
  if (loader == NULL || out_action == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_action = H2_LOADER_STARTUP_ACTION_COMMAND_MODE;
  rc = mount_file_points(loader);
  if (rc != H2_PAL_OK)
    return fail_recovery(loader, rc);
  rc = h2_loader_read_status(loader, &loader->status);
  if (rc != H2_PAL_OK)
    return fail_recovery(loader, rc);
  rc = seed_running_metadata(loader);
  if (rc != H2_PAL_OK)
    return fail_recovery(loader, rc);
  if (loader->config.confirm_active_image != NULL) {
    rc = loader->config.confirm_active_image(loader->config.confirm_user);
    if (rc != H2_PAL_OK)
      return fail_recovery(loader, rc);
  }

  if (loader->config.active_identity.role != H2_LOADER_IMAGE_ROLE_H2LOADER) {
    return H2_PAL_OK;
  }
  if (loader->status.running_partition_id == loader->config.app_partition_id) {
    rc = copy_partition_2_to_1(loader);
    if (rc != H2_PAL_OK)
      return fail_recovery(loader, rc);
    *out_action = H2_LOADER_STARTUP_ACTION_REBOOTING_H2LOADER;
    return H2_PAL_OK;
  }
  if (loader->status.running_partition_id !=
      loader->config.h2loader_partition_id) {
    return fail_recovery(loader, H2_PAL_ERR_INVALID_STATE);
  }
  if (loader->force_command_mode ||
      loader->status.boot_intent == H2_LOADER_BOOT_INTENT_LOADER) {
    return H2_PAL_OK;
  }
  if (loader->status.boot_intent != H2_LOADER_BOOT_INTENT_AUTO) {
    return fail_recovery(loader, H2_PAL_ERR_FORMAT);
  }

  rc = finish_loader_stage_if_converged(loader);
  if (rc != H2_PAL_OK)
    return fail_recovery(loader, rc);
  if (loader->status.stage.valid) {
    rc = inspect_current_stage(loader, &inspection);
    if (rc != H2_PAL_OK)
      return fail_recovery(loader, rc);
    rc = write_partition_2(loader, &inspection);
    if (rc != H2_PAL_OK)
      return fail_recovery(loader, rc);
  }
  if (!loader->status.partition_2.valid ||
      h2_loader_metadata_image_equal(&loader->status.partition_1,
                                     &loader->status.partition_2)) {
    rc = finish_loader_stage_if_converged(loader);
    return rc == H2_PAL_OK ? H2_PAL_OK : fail_recovery(loader, rc);
  }
  if (!mfg_gate_satisfied(loader))
    return H2_PAL_OK;
  emit_event(loader, H2_LOADER_STARTUP_EVENT_BOOT_PARTITION_2, H2_PAL_OK);
  rc = set_next_and_reboot(loader, loader->config.app_partition_id,
                           H2_LOADER_BOOT_INTENT_AUTO,
                           H2_LOADER_DISRUPTIVE_REBOOT_APP, NULL, NULL);
  if (rc != H2_PAL_OK)
    return fail_recovery(loader, rc);
  *out_action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
  return H2_PAL_OK;
}

int h2_loader_reboot_h2loader_with_transition(
    h2_loader_t *loader, h2_loader_reboot_transition_fn transition,
    void *transition_user) {
  if (loader == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return set_next_and_reboot(loader, loader->config.h2loader_partition_id,
                             H2_LOADER_BOOT_INTENT_LOADER,
                             H2_LOADER_DISRUPTIVE_REBOOT_LOADER, transition,
                             transition_user);
}

int h2_loader_reboot_app_with_transition(
    h2_loader_t *loader, h2_loader_reboot_transition_fn transition,
    void *transition_user) {
  if (loader == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return set_next_and_reboot(
      loader, loader->config.app_partition_id, H2_LOADER_BOOT_INTENT_AUTO,
      H2_LOADER_DISRUPTIVE_REBOOT_APP, transition, transition_user);
}

int h2_loader_reboot_upgrade_with_transition(
    h2_loader_t *loader, h2_loader_reboot_transition_fn transition,
    void *transition_user) {
  if (loader == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return set_next_and_reboot(
      loader, loader->config.h2loader_partition_id, H2_LOADER_BOOT_INTENT_AUTO,
      H2_LOADER_DISRUPTIVE_REBOOT_UPGRADE, transition, transition_user);
}
