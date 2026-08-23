#include "h2_h2loader_host_flash.h"

static int recovery_cancelled(
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user) {
    return is_cancelled != NULL && is_cancelled(cancel_user);
}

h2_pal_result_t h2_h2loader_host_recovery_validate(
    const h2_h2loader_host_recovery_authorization_t *authorization,
    const h2_h2loader_host_catalog_entry_t *asset,
    uint64_t now_ms) {
    if (authorization == NULL || asset == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (authorization->transport != H2_H2LOADER_HOST_TRANSPORT_SERIAL ||
        asset->operation != H2_H2LOADER_HOST_ASSET_OPERATION_RECOVERY ||
        asset->role != H2_H2LOADER_HOST_ASSET_ROLE_LOADER ||
        authorization->identity_confirmed == 0u ||
        authorization->destructive_confirmed == 0u ||
        authorization->probe_completed_ms == 0u ||
        authorization->expires_ms < authorization->probe_completed_ms ||
        now_ms < authorization->probe_completed_ms ||
        now_ms > authorization->expires_ms) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (authorization->reason ==
            H2_H2LOADER_HOST_RECOVERY_PROBE_FAILED) {
        if (authorization->probe_result != H2_PAL_ERR_TIMEOUT ||
            authorization->probe_attempts < 2u) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        return H2_PAL_OK;
    }
    if (authorization->reason ==
            H2_H2LOADER_HOST_RECOVERY_BLANK_FIXTURE) {
        return authorization->probe_result == H2_PAL_ERR_TIMEOUT &&
                authorization->probe_attempts >= 1u
            ? H2_PAL_OK
            : H2_PAL_ERR_INVALID_STATE;
    }
    return H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t h2_h2loader_host_recovery_run(
    const h2_h2loader_host_recovery_authorization_t *authorization,
    const h2_h2loader_host_catalog_entry_t *asset,
    uint64_t now_ms,
    const h2_h2loader_host_flash_driver_t *driver,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user,
    h2_h2loader_host_progress_fn on_progress,
    void *progress_user) {
    h2_pal_result_t rc;
    h2_pal_result_t close_rc;

    if (driver == NULL || driver->vtable == NULL ||
        driver->vtable->prepare == NULL ||
        driver->vtable->erase == NULL ||
        driver->vtable->write == NULL ||
        driver->vtable->verify == NULL ||
        driver->vtable->reset_to_loader == NULL ||
        driver->vtable->close == NULL ||
        read_payload == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_h2loader_host_recovery_validate(authorization, asset, now_ms);
    if (rc != H2_PAL_OK) {
        (void)driver->vtable->close(driver->user);
        return rc;
    }
    if (recovery_cancelled(is_cancelled, cancel_user)) {
        (void)driver->vtable->close(driver->user);
        return H2_PAL_EXIT;
    }
    rc = driver->vtable->prepare(
        driver->user, asset, read_payload, payload_user);
    if (rc != H2_PAL_OK) {
        (void)driver->vtable->close(driver->user);
        return rc;
    }
    rc = driver->vtable->erase(
        driver->user, is_cancelled, cancel_user);
    if (rc == H2_PAL_OK &&
        !recovery_cancelled(is_cancelled, cancel_user)) {
        rc = driver->vtable->write(
            driver->user,
            asset,
            read_payload,
            payload_user,
            is_cancelled,
            cancel_user,
            on_progress,
            progress_user);
    }
    if (rc == H2_PAL_OK &&
        recovery_cancelled(is_cancelled, cancel_user)) {
        rc = H2_PAL_EXIT;
    }
    if (rc == H2_PAL_OK) {
        rc = driver->vtable->verify(
            driver->user, is_cancelled, cancel_user);
    }
    if (rc == H2_PAL_OK) {
        rc = driver->vtable->reset_to_loader(driver->user);
    }
    close_rc = driver->vtable->close(driver->user);
    return rc == H2_PAL_OK ? close_rc : rc;
}
