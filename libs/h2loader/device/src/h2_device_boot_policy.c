#include "h2_device_boot_policy.h"
#include "jieli_pending_boot.h"
#include <stdio.h>
#include <string.h>

static int read_status(h2_device_boot_policy_t* self, h2_loader_status_t* out) {
  return h2_loader_read_pref_status(self->config.pref, self->config.allocator,
                                    out);
}

int h2_device_boot_policy_clear_trial(const h2_pal_pref_api_t* pref) {
  static const char* const keys[] = {
      H2_JIELI_TRIAL_ATTEMPT_KEY,
      H2_JIELI_TRIAL_CHECKSUM_KEY,
      H2_JIELI_TRIAL_RESET_REASON_KEY,
  };
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(pref, H2_LOADER_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  for (size_t i = 0u; rc == H2_PAL_OK && i < sizeof(keys) / sizeof(keys[0]);
       ++i) {
    rc = ns->remove(ns, keys[i]);
    if (rc == H2_PAL_ERR_NOT_FOUND) rc = H2_PAL_OK;
  }
  if (rc == H2_PAL_OK) rc = ns->commit(ns);
  if (ns != NULL && ns->close != NULL) {
    int closed = ns->close(ns);
    if (rc == H2_PAL_OK) rc = closed;
  }
  return rc;
}

static int set_trial_attempt(h2_device_boot_policy_t* self, int present) {
  h2_loader_status_t status;
  int rc = read_status(self, &status);
  if (rc != H2_PAL_OK) return rc;
  if (present && (!status.partition_2.valid ||
                  (status.partition_2.role != H2_LOADER_IMAGE_ROLE_APP &&
                   status.partition_2.role != H2_LOADER_IMAGE_ROLE_H2LOADER)))
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_pref_namespace_t* ns = NULL;
  rc = h2_pal_pref_open(self->config.pref, H2_LOADER_PREF_NAMESPACE,
                        H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc != H2_PAL_OK) return rc;
  rc = present ? ns->set_string(ns, H2_JIELI_TRIAL_ATTEMPT_KEY,
                                status.partition_2.image_checksum)
               : ns->remove(ns, H2_JIELI_TRIAL_ATTEMPT_KEY);
  if (rc == H2_PAL_ERR_NOT_FOUND && !present) rc = H2_PAL_OK;
  if (rc == H2_PAL_OK) rc = ns->commit(ns);
  int closed = ns->close(ns);
  return rc == H2_PAL_OK ? closed : rc;
}

static int reconcile_trial_state(h2_device_boot_policy_t* self) {
  h2_loader_status_t status;
  int rc = read_status(self, &status);
  if (rc != H2_PAL_OK) return rc;
  h2_pal_pref_namespace_t* ns = NULL;
  rc = h2_pal_pref_open(self->config.pref, H2_LOADER_PREF_NAMESPACE,
                        H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  if (rc == H2_PAL_ERR_NOT_FOUND) return H2_PAL_OK;
  if (rc != H2_PAL_OK) return rc;
  char* attempt = NULL;
  rc = ns->get_string(ns, self->config.allocator, H2_JIELI_TRIAL_ATTEMPT_KEY,
                      &attempt);
  int attempted = rc == H2_PAL_OK && attempt != NULL &&
                  status.partition_2.valid &&
                  (status.partition_2.role == H2_LOADER_IMAGE_ROLE_APP ||
                   status.partition_2.role == H2_LOADER_IMAGE_ROLE_H2LOADER) &&
                  strcmp(attempt, status.partition_2.image_checksum) == 0;
  if (attempt != NULL) h2_pal_mem_free(self->config.allocator, attempt);
  int closed = ns->close(ns);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) return rc;
  if (closed != H2_PAL_OK) return closed;
  self->app_trial_rolled_back =
      self->running_partition_id == self->config.loader_partition_id &&
      attempted;
  const int staged =
      status.stage.valid && status.partition_2.valid &&
      h2_loader_metadata_image_equal(&status.stage, &status.partition_2);
  /* A restaged old image is not an attempted boot. A different new Stage must
   * not erase evidence that the installed image failed to confirm. */
  return !staged && !attempted
             ? h2_device_boot_policy_clear_trial(self->config.pref)
             : H2_PAL_OK;
}

static int pending_boot_save(h2_device_boot_policy_t* self,
                             const h2_loader_metadata_t* candidate,
                             const h2_pal_power_native_evidence_t* evidence) {
  uint8_t encoded[H2_JIELI_PENDING_BOOT_WIRE_SIZE];
  if (!candidate->valid || candidate->role != H2_LOADER_IMAGE_ROLE_H2LOADER ||
      !h2_jieli_pending_boot_encode(evidence, candidate->image_checksum,
                                    encoded))
    return H2_PAL_ERR_FORMAT;
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(self->config.pref, H2_LOADER_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc != H2_PAL_OK) return rc;
  rc = ns->set_blob == NULL || ns->commit == NULL
           ? H2_PAL_ERR_UNSUPPORTED
           : ns->set_blob(ns, H2_JIELI_PENDING_BOOT_KEY, encoded,
                          sizeof(encoded));
  if (rc == H2_PAL_OK) rc = ns->commit(ns);
  int closed = ns->close(ns);
  return rc == H2_PAL_OK ? closed : rc;
}

static int pending_boot_load(h2_device_boot_policy_t* self,
                             h2_pal_power_native_evidence_t* evidence) {
  h2_loader_status_t status;
  int rc = read_status(self, &status);
  if (rc != H2_PAL_OK) return rc;
  if (!status.partition_2.valid ||
      status.partition_2.role != H2_LOADER_IMAGE_ROLE_H2LOADER)
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_pref_namespace_t* ns = NULL;
  rc = h2_pal_pref_open(self->config.pref, H2_LOADER_PREF_NAMESPACE,
                        H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  if (rc != H2_PAL_OK) return rc;
  void* blob = NULL;
  size_t size = 0u;
  rc = ns->get_blob == NULL
           ? H2_PAL_ERR_UNSUPPORTED
           : ns->get_blob(ns, self->config.allocator, H2_JIELI_PENDING_BOOT_KEY,
                          &blob, &size);
  if (rc == H2_PAL_OK &&
      !h2_jieli_pending_boot_decode(
          blob, size, status.partition_2.image_checksum, evidence))
    rc = H2_PAL_ERR_FORMAT;
  if (blob != NULL) h2_pal_mem_free(self->config.allocator, blob);
  int closed = ns->close(ns);
  return rc == H2_PAL_OK ? closed : rc;
}

static int shadow_allowed(h2_device_boot_policy_t* self, uint32_t id) {
  return (self->config.flags & H2_DEVICE_BOOT_STORED_SHADOW) == 0u ||
         id != self->config.app_partition_id ||
         self->partition_2_shadow_reusable ||
         (self->finished_partition_id == id && !self->candidate_activated);
}

static int image_info(void* user, uint32_t id, h2_pal_image_view_t view,
                      h2_pal_image_info_t* out) {
  h2_device_boot_policy_t* self = user;
  int rc = h2_pal_image_get_info(self->config.image, id, view, out);
  if (rc == H2_PAL_OK && view == H2_PAL_IMAGE_VIEW_STORED &&
      !shadow_allowed(self, id)) {
    out->flags &= ~(H2_PAL_IMAGE_FLAG_READABLE | H2_PAL_IMAGE_FLAG_SIZE_KNOWN);
    out->image_size = 0u;
  }
  return rc;
}

static int image_read(void* user, uint32_t id, h2_pal_image_view_t view,
                      uint64_t offset, void* data, size_t len) {
  h2_device_boot_policy_t* self = user;
  if (view == H2_PAL_IMAGE_VIEW_STORED && !shadow_allowed(self, id))
    return H2_PAL_ERR_NOT_FOUND;
  return h2_pal_image_read(self->config.image, id, view, offset, data, len);
}

static int image_begin(void* user,
                       const h2_pal_image_write_request_t* request) {
  h2_device_boot_policy_t* self = user;
  int rc = h2_pal_image_begin(self->config.image, request);
  if (rc == H2_PAL_OK) {
    self->writing_partition_id = request->partition_id;
    self->finished_partition_id = 0u;
    self->candidate_activated = 0;
  }
  return rc;
}
static int image_write(void* user, const void* data, size_t len) {
  return h2_pal_image_write(((h2_device_boot_policy_t*)user)->config.image,
                            data, len);
}
static int image_finish(void* user) {
  h2_device_boot_policy_t* self = user;
  int rc = h2_pal_image_finish(self->config.image);
  if (rc == H2_PAL_OK) {
    self->finished_partition_id = self->writing_partition_id;
    self->writing_partition_id = 0u;
  }
  return rc;
}
static int image_abort(void* user) {
  h2_device_boot_policy_t* self = user;
  int rc = h2_pal_image_abort(self->config.image);
  self->writing_partition_id = self->finished_partition_id = 0u;
  self->candidate_activated = 0;
  return rc;
}

static void policy_partition(h2_device_boot_policy_t* self,
                             h2_pal_power_boot_partition_t* partition) {
  if (partition->id == self->config.app_partition_id &&
      self->app_trial_rolled_back)
    partition->flags &= ~H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE;
  (void)snprintf(
      partition->name, sizeof(partition->name), "%s",
      partition->id == self->config.loader_partition_id ? "h2loader" : "app");
}
struct list_context {
  h2_device_boot_policy_t* self;
  h2_pal_power_boot_partition_cb_t callback;
  void* user;
};
static int list_partition(void* user,
                          const h2_pal_power_boot_partition_t* native) {
  struct list_context* context = user;
  h2_pal_power_boot_partition_t partition = *native;
  policy_partition(context->self, &partition);
  return context->callback(context->user, &partition);
}
static int power_list(void* user, h2_pal_power_boot_partition_cb_t callback,
                      void* callback_user) {
  h2_device_boot_policy_t* self = user;
  struct list_context context = {self, callback, callback_user};
  return h2_pal_power_list_boot_partitions(self->config.power, list_partition,
                                           &context);
}
static int power_running(void* user, h2_pal_power_boot_partition_t* out) {
  h2_device_boot_policy_t* self = user;
  int rc = h2_pal_power_get_running_boot_partition(self->config.power, out);
  if (rc == H2_PAL_OK) policy_partition(self, out);
  return rc;
}
static int power_next(void* user, h2_pal_power_boot_partition_t* out) {
  h2_device_boot_policy_t* self = user;
  int rc = h2_pal_power_get_next_boot_partition(self->config.power, out);
  if (rc == H2_PAL_OK) policy_partition(self, out);
  return rc;
}
static int power_capabilities(void* user, h2_pal_power_capabilities_t* out) {
  return h2_pal_power_get_capabilities(
      ((h2_device_boot_policy_t*)user)->config.power, out);
}
static int power_boot_info(void* user, h2_pal_power_boot_info_t* out) {
  return h2_pal_power_get_boot_info(
      ((h2_device_boot_policy_t*)user)->config.power, out);
}
static int power_state(void* user, h2_pal_power_state_t* out) {
  return h2_pal_power_get_state(((h2_device_boot_policy_t*)user)->config.power,
                                out);
}

static int power_set_next(void* user, uint32_t id) {
  h2_device_boot_policy_t* self = user;
  int rc;
  if ((self->config.flags & H2_DEVICE_BOOT_TRACK_TRIAL) == 0u) {
    if ((self->config.flags & H2_DEVICE_BOOT_PREPARE_SELECTION) != 0u ||
        (id == self->config.app_partition_id &&
         (self->config.flags & H2_DEVICE_BOOT_PREPARE_APP_RESTART) != 0u)) {
      h2_pal_power_boot_request_t request = {
          .partition_id = id,
          .policy = H2_PAL_POWER_BOOT_POLICY_EXISTING,
      };
      if (id == self->config.app_partition_id &&
          (self->finished_partition_id == id ||
           (self->config.flags & H2_DEVICE_BOOT_PREPARE_APP_RESTART) != 0u)) {
        h2_loader_status_t status;
        rc = read_status(self, &status);
        if (rc != H2_PAL_OK) return rc;
        if (!status.partition_2.valid) return H2_PAL_ERR_INVALID_STATE;
        request.policy =
            status.partition_2.role == H2_LOADER_IMAGE_ROLE_H2LOADER
                ? H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY
                : H2_PAL_POWER_BOOT_POLICY_TRIAL;
      }
      h2_pal_power_native_evidence_t evidence;
      rc = h2_pal_power_prepare_next_boot_partition(self->config.power,
                                                    &request, &evidence);
      if (rc != H2_PAL_OK) return rc;
    }
    rc = h2_pal_power_set_next_boot_partition(self->config.power, id);
    if (rc == H2_PAL_OK) self->candidate_activated = 1;
    return rc;
  }
  if (id == self->running_partition_id) {
    if (id == self->config.app_partition_id &&
        !self->loader_candidate_confirmed) {
      h2_loader_status_t status;
      rc = read_status(self, &status);
      if (rc != H2_PAL_OK) return rc;
      /* Restarting an installed App retains the native next=running mapping;
       * it never invokes the recovery-publication barrier used by Loader copy.
       */
      if (status.partition_2.valid &&
          status.partition_2.role == H2_LOADER_IMAGE_ROLE_APP)
        return set_trial_attempt(self, 1);
    }
    if (id == self->config.app_partition_id &&
        !self->loader_candidate_confirmed)
      return H2_PAL_ERR_INVALID_STATE;
    rc = h2_pal_power_set_next_boot_partition(self->config.power, id);
    /* Publish the recovery header before clearing durable trial evidence. */
    if (rc == H2_PAL_OK && id == self->config.app_partition_id)
      rc = set_trial_attempt(self, 0);
    return rc;
  }
  h2_pal_power_boot_request_t request = {
      .partition_id = id,
      .policy = H2_PAL_POWER_BOOT_POLICY_EXISTING,
  };
  h2_loader_status_t status;
  if (id == self->config.app_partition_id) {
    rc = read_status(self, &status);
    if (rc != H2_PAL_OK) return rc;
    if (!status.partition_2.valid ||
        (status.partition_2.role != H2_LOADER_IMAGE_ROLE_APP &&
         status.partition_2.role != H2_LOADER_IMAGE_ROLE_H2LOADER))
      return H2_PAL_ERR_INVALID_STATE;
    if (self->finished_partition_id == id && !self->candidate_activated) {
      request.policy = status.partition_2.role == H2_LOADER_IMAGE_ROLE_APP
                           ? H2_PAL_POWER_BOOT_POLICY_TRIAL
                           : H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY;
    } else if (self->app_trial_rolled_back ||
               status.partition_2.role != H2_LOADER_IMAGE_ROLE_APP) {
      return H2_PAL_ERR_INVALID_STATE;
    }
    rc = set_trial_attempt(self, 1);
    if (rc != H2_PAL_OK) return rc;
  }
  h2_pal_power_native_evidence_t evidence;
  rc = h2_pal_power_prepare_next_boot_partition(self->config.power, &request,
                                                &evidence);
  if (rc == H2_PAL_OK &&
      request.policy == H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY &&
      (self->config.flags & H2_DEVICE_BOOT_PERSIST_RECOVERY) != 0u)
    rc = pending_boot_save(self, &status.partition_2, &evidence);
  if (rc != H2_PAL_OK) {
    /* A failed pending commit cannot activate a candidate. Public abort is
     * intentional here: no selection has crossed the recovery barrier yet. */
    if (self->finished_partition_id == id && !self->candidate_activated)
      (void)image_abort(self);
    if (id == self->config.app_partition_id) (void)set_trial_attempt(self, 0);
    return rc;
  }
  rc = h2_pal_power_set_next_boot_partition(self->config.power, id);
  if (rc == H2_PAL_OK) {
    self->candidate_activated = 1;
    self->app_trial_rolled_back = 0;
    self->partition_2_shadow_reusable =
        id == self->config.app_partition_id &&
        request.policy == H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY;
  }
  return rc;
}
static int power_feed_watchdog(void* user) {
  return h2_pal_power_feed_watchdog(
      ((h2_device_boot_policy_t*)user)->config.power);
}
static int power_reboot(void* user, uint32_t reason) {
  return h2_pal_power_reboot(((h2_device_boot_policy_t*)user)->config.power,
                             reason);
}
static int power_prepare(void* user, const h2_pal_power_boot_request_t* request,
                         h2_pal_power_native_evidence_t* evidence) {
  return h2_pal_power_prepare_next_boot_partition(
      ((h2_device_boot_policy_t*)user)->config.power, request, evidence);
}
static int power_image_state(void* user, uint32_t id,
                             h2_pal_power_image_state_t* out) {
  h2_device_boot_policy_t* self = user;
  int rc = h2_pal_power_get_image_state(self->config.power, id, out);
  if (rc == H2_PAL_OK && id == self->config.app_partition_id &&
      self->app_trial_rolled_back)
    out->state = H2_PAL_POWER_IMAGE_STATE_ABORTED;
  return rc;
}
static int power_confirm(void* user,
                         const h2_pal_power_native_evidence_t* evidence,
                         h2_pal_power_image_confirmation_t* out) {
  return h2_pal_power_confirm_running_image(
      ((h2_device_boot_policy_t*)user)->config.power, evidence, out);
}

int h2_device_boot_policy_confirm(void* user) {
  h2_device_boot_policy_t* self = user;
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_power_native_evidence_t evidence;
  const h2_pal_power_native_evidence_t* borrowed = NULL;
  if (self->running_partition_id == self->config.app_partition_id &&
      (self->config.flags & H2_DEVICE_BOOT_PERSIST_RECOVERY) != 0u) {
    int rc = pending_boot_load(self, &evidence);
    if (rc == H2_PAL_OK)
      borrowed = &evidence;
    else if (rc != H2_PAL_ERR_NOT_FOUND)
      return rc;
  }
  h2_pal_power_image_confirmation_t confirmation;
  int rc = h2_pal_power_confirm_running_image(self->config.power, borrowed,
                                              &confirmation);
  if (rc == H2_PAL_OK &&
      self->running_partition_id == self->config.app_partition_id &&
      (self->config.flags & H2_DEVICE_BOOT_PERSIST_RECOVERY) != 0u) {
    if ((confirmation.flags & H2_PAL_POWER_IMAGE_FLAG_RECOVERY_READY) == 0u)
      return H2_PAL_ERR_INVALID_STATE;
    self->loader_candidate_confirmed = 1;
  }
  return rc;
}

int h2_device_boot_policy_init(h2_device_boot_policy_t* self,
                               const h2_device_boot_policy_config_t* config) {
  if (self == NULL || config == NULL || config->image == NULL ||
      config->power == NULL || config->pref == NULL ||
      config->allocator == NULL || config->fs == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(self, 0, sizeof(*self));
  self->config = *config;
  h2_pal_power_boot_partition_t running;
  int rc = h2_pal_power_get_running_boot_partition(config->power, &running);
  if (rc != H2_PAL_OK) return rc;
  self->running_partition_id = running.id;
  h2_loader_status_t status;
  rc = read_status(self, &status);
  if (rc != H2_PAL_OK) return rc;
  self->partition_2_shadow_reusable =
      status.partition_2.valid &&
      status.partition_2.role == H2_LOADER_IMAGE_ROLE_H2LOADER;
  rc = (config->flags & H2_DEVICE_BOOT_TRACK_TRIAL) != 0u
           ? reconcile_trial_state(self)
           : H2_PAL_OK;
  if (rc != H2_PAL_OK) return rc;
  static const h2_pal_image_vtable_t image_vtable = {
      .get_info = image_info,
      .read = image_read,
      .begin = image_begin,
      .write = image_write,
      .finish = image_finish,
      .abort = image_abort,
  };
  static const h2_pal_power_vtable_t power_vtable = {
      .get_capabilities = power_capabilities,
      .get_boot_info = power_boot_info,
      .get_state = power_state,
      .list_boot_partitions = power_list,
      .get_running_boot_partition = power_running,
      .get_next_boot_partition = power_next,
      .set_next_boot_partition = power_set_next,
      .reboot = power_reboot,
      .prepare_next_boot_partition = power_prepare,
      .get_image_state = power_image_state,
      .confirm_running_image = power_confirm,
      .feed_watchdog = power_feed_watchdog,
  };
  self->image = (h2_pal_image_api_t){.user = self, .vtable = &image_vtable};
  self->power = (h2_pal_power_api_t){.user = self, .vtable = &power_vtable};
  const h2_loader_pal_config_t adapter = {
      .image = &self->image,
      .power = &self->power,
      .time = config->time,
      .allocator = config->allocator,
      .loader_partition_id = config->loader_partition_id,
      .app_partition_id = config->app_partition_id,
      .digest_yield_ms = config->digest_yield_ms,
      .digest_feed_watchdog = config->digest_feed_watchdog,
  };
  return h2_loader_pal_init(&self->pal, &adapter);
}

int h2_device_boot_policy_confirm_app(void* user) {
  h2_device_boot_policy_t* self = user;
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_power_image_confirmation_t confirmation;
  int rc = h2_pal_power_confirm_running_image(self->config.power, NULL,
                                              &confirmation);
  if (rc == H2_PAL_OK &&
      (self->config.flags & H2_DEVICE_BOOT_TRACK_TRIAL) != 0u)
    rc = h2_device_boot_policy_clear_trial(self->config.pref);
  return rc;
}

static void identity_from_metadata(const h2_loader_metadata_t* metadata,
                                   h2_loader_image_identity_t* out) {
  memset(out, 0, sizeof(*out));
  out->format = 1u;
  out->role = metadata->role;
  out->image_size = metadata->image_size;
  memcpy(out->board, metadata->board, sizeof(out->board));
  memcpy(out->target, metadata->target, sizeof(out->target));
  memcpy(out->version, metadata->version, sizeof(out->version));
  memcpy(out->image_sha256, metadata->image_checksum,
         sizeof(out->image_sha256));
}

static int padded_identity(h2_device_boot_policy_t* self,
                           h2_loader_image_role_t role, const char* board,
                           const char* target, const char* version,
                           h2_loader_image_identity_t* out) {
  /* Identity hashing never resets a Stage/command digest between its chunks. */
  h2_loader_pal_t independent;
  int rc = h2_loader_pal_init(&independent, &self->pal.config);
  if (rc != H2_PAL_OK) return rc;
  const h2_loader_config_t config = {
      .pref = self->config.pref,
      .power = &self->power,
      .package = {.allocator = self->config.allocator,
                  .image_reader = &independent.image_reader,
                  .digest = independent.digest},
      .board = board,
      .target = target,
      .h2loader_partition_id = self->config.loader_partition_id,
      .app_partition_id = self->config.app_partition_id,
  };
  if (role == H2_LOADER_IMAGE_ROLE_H2LOADER)
    return h2_loader_read_current_loader_identity(&config, version, out);
  h2_loader_status_t status;
  rc = read_status(self, &status);
  if (rc != H2_PAL_OK) return rc;
  const h2_loader_metadata_t* active = &status.partition_2;
  if (!active->valid || active->role != role ||
      strcmp(active->version, version) != 0 ||
      strcmp(active->board, board) != 0 || strcmp(active->target, target) != 0)
    active = NULL;
  h2_pal_image_info_t info;
  rc = h2_pal_image_get_info(self->config.image, self->running_partition_id,
                             H2_PAL_IMAGE_VIEW_STORED, &info);
  if (rc != H2_PAL_OK) return rc;
  uint64_t size = active != NULL ? active->image_size : info.capacity;
  if (size == 0u || size > info.capacity) return H2_PAL_ERR_FORMAT;
  uint8_t* buffer = h2_pal_mem_alloc(self->config.allocator, 4096u);
  if (buffer == NULL) return H2_PAL_ERR_NO_MEMORY;
  uint8_t digest[32];
  rc = independent.digest.start(independent.digest.user);
  for (uint64_t offset = 0u; rc == H2_PAL_OK && offset < size;) {
    size_t take = size - offset > 4096u ? 4096u : (size_t)(size - offset);
    rc = h2_pal_image_read(self->config.image, self->running_partition_id,
                           H2_PAL_IMAGE_VIEW_STORED, offset, buffer, take);
    if (rc == H2_PAL_OK)
      rc = independent.digest.update(independent.digest.user, buffer, take);
    offset += take;
  }
  if (rc == H2_PAL_OK)
    rc = independent.digest.finish(independent.digest.user, digest);
  independent.digest.abort(independent.digest.user);
  h2_pal_mem_free(self->config.allocator, buffer);
  if (rc != H2_PAL_OK) return rc;
  memset(out, 0, sizeof(*out));
  h2_loader_sha256_hex(digest, out->image_sha256);
  if (active != NULL && strcmp(active->image_checksum, out->image_sha256) != 0)
    return H2_PAL_ERR_FORMAT;
  out->format = 1u;
  out->role = role;
  out->image_size = size;
  (void)snprintf(out->board, sizeof(out->board), "%s", board);
  (void)snprintf(out->target, sizeof(out->target), "%s", target);
  (void)snprintf(out->version, sizeof(out->version), "%s", version);
  return H2_PAL_OK;
}

int h2_device_boot_policy_current_identity(h2_device_boot_policy_t* self,
                                           h2_loader_image_role_t role,
                                           const char* board,
                                           const char* target,
                                           const char* version,
                                           h2_loader_image_identity_t* out) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (self == NULL || board == NULL || target == NULL || version == NULL ||
      out == NULL || board[0] == '\0' || target[0] == '\0' ||
      version[0] == '\0' || strlen(board) >= sizeof(out->board) ||
      strlen(target) >= sizeof(out->target) ||
      strlen(version) >= sizeof(out->version) ||
      (role != H2_LOADER_IMAGE_ROLE_APP &&
       role != H2_LOADER_IMAGE_ROLE_H2LOADER))
    return H2_PAL_ERR_INVALID_ARG;
  h2_device_identity_policy_t policy = role == H2_LOADER_IMAGE_ROLE_APP
                                           ? self->config.app_identity
                                           : self->config.loader_identity;
  if (policy == H2_DEVICE_IDENTITY_PADDED_WINDOW)
    return padded_identity(self, role, board, target, version, out);
  if (policy == H2_DEVICE_IDENTITY_COMMITTED) {
    h2_loader_status_t status;
    int rc = read_status(self, &status);
    if (rc != H2_PAL_OK) return rc;
    h2_loader_metadata_slot_t slot =
        self->running_partition_id == self->config.loader_partition_id
            ? H2_LOADER_METADATA_SLOT_PARTITION_1
            : H2_LOADER_METADATA_SLOT_PARTITION_2;
    const h2_loader_metadata_t* persisted =
        slot == H2_LOADER_METADATA_SLOT_PARTITION_1 ? &status.partition_1
                                                    : &status.partition_2;
    if (persisted->valid && persisted->role == role &&
        persisted->image_size != 0u && strcmp(persisted->board, board) == 0 &&
        strcmp(persisted->target, target) == 0 &&
        h2_loader_metadata_validate(slot, persisted) == H2_PAL_OK) {
      identity_from_metadata(persisted, out);
      return H2_PAL_OK;
    }
    if (role == H2_LOADER_IMAGE_ROLE_APP) return H2_PAL_ERR_INVALID_STATE;
  }
  h2_loader_pal_t independent;
  h2_loader_pal_config_t identity_config = self->pal.config;
  identity_config.identity_view = policy == H2_DEVICE_IDENTITY_NATIVE
                                      ? H2_PAL_IMAGE_VIEW_NATIVE
                                      : H2_PAL_IMAGE_VIEW_STORED;
  int rc = h2_loader_pal_init(&independent, &identity_config);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_loader_pal_current_image_identity(&independent, role, board, target,
                                            version, out);
  if ((rc != H2_PAL_ERR_UNSUPPORTED && rc != H2_PAL_ERR_NOT_FOUND) ||
      role != H2_LOADER_IMAGE_ROLE_H2LOADER ||
      (self->config.flags & H2_DEVICE_BOOT_FACTORY_IDENTITY) == 0u)
    return rc;
  h2_pal_image_info_t native_info;
  rc = h2_pal_image_get_info(self->config.image, self->running_partition_id,
                             H2_PAL_IMAGE_VIEW_NATIVE, &native_info);
  if (rc != H2_PAL_OK) return rc;
  if ((native_info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN) == 0u ||
      native_info.image_size == 0u)
    return H2_PAL_ERR_UNSUPPORTED;
  const h2_pal_power_boot_request_t request = {
      .partition_id = self->running_partition_id,
      .policy = H2_PAL_POWER_BOOT_POLICY_EXISTING,
  };
  h2_pal_power_native_evidence_t evidence;
  rc = h2_pal_power_prepare_next_boot_partition(self->config.power, &request,
                                                &evidence);
  if (rc != H2_PAL_OK) return rc;
  if (evidence.format != 2u || evidence.size == 0u ||
      evidence.size > sizeof(evidence.bytes))
    return H2_PAL_ERR_UNSUPPORTED;
  /* Compatibility factory identity: opaque facts, version, board, target in
   * precisely this order. This is never a UFW content hash. */
  h2_loader_sha256_t sha;
  uint8_t digest[32];
  h2_loader_sha256_init(&sha);
  h2_loader_sha256_update(&sha, evidence.bytes, evidence.size);
  h2_loader_sha256_update(&sha, (const uint8_t*)version, strlen(version));
  h2_loader_sha256_update(&sha, (const uint8_t*)board, strlen(board));
  h2_loader_sha256_update(&sha, (const uint8_t*)target, strlen(target));
  h2_loader_sha256_finish(&sha, digest);
  out->format = 1u;
  out->role = role;
  out->image_size = native_info.image_size;
  (void)snprintf(out->board, sizeof(out->board), "%s", board);
  (void)snprintf(out->target, sizeof(out->target), "%s", target);
  (void)snprintf(out->version, sizeof(out->version), "%s", version);
  h2_loader_sha256_hex(digest, out->image_sha256);
  return H2_PAL_OK;
}

static int legacy_app_begin(void* user, const h2_bundle_entry_t* entry) {
  h2_device_boot_policy_t* self = user;
  if (entry == NULL || self->config.app_entry_path == NULL ||
      strcmp(entry->path, self->config.app_entry_path) != 0)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_image_write_request_t request = {
      .partition_id = self->config.app_partition_id,
      .execution_partition_id = self->config.app_partition_id,
      .image_size = entry->size,
  };
  return image_begin(self, &request);
}
static int legacy_app_write(void* user, const h2_bundle_entry_t* entry,
                            const void* data, size_t len) {
  (void)entry;
  return image_write(user, data, len);
}
static int legacy_app_finish(void* user, const h2_bundle_entry_t* entry) {
  h2_device_boot_policy_t* self = user;
  (void)entry;
  int rc = image_finish(self);
  if (rc != H2_PAL_OK) return rc;
  const h2_pal_power_boot_request_t request = {
      .partition_id = self->config.app_partition_id,
      .policy = H2_PAL_POWER_BOOT_POLICY_TRIAL,
  };
  h2_pal_power_native_evidence_t evidence;
  return h2_pal_power_prepare_next_boot_partition(self->config.power, &request,
                                                  &evidence);
}
static void legacy_app_abort(void* user) { (void)image_abort(user); }
const h2_bundle_app_writer_t* h2_device_boot_policy_legacy_app_writer(
    h2_device_boot_policy_t* self) {
  if (self == NULL) return NULL;
  self->legacy_app_writer = (h2_bundle_app_writer_t){
      .user = self,
      .begin = legacy_app_begin,
      .write = legacy_app_write,
      .end = legacy_app_finish,
      .abort = legacy_app_abort,
  };
  return &self->legacy_app_writer;
}
