#include "h2_h2loader_host_scheduler.h"

#include "h2_h2loader_host_internal.h"

#include <stdio.h>
#include <string.h>

struct h2_h2loader_host_scheduler {
    const h2_pal_mem_api_t *allocator;
    h2_h2loader_host_job_result_t *jobs;
    size_t job_count;
    size_t max_concurrency;
    size_t active_count;
    int paused;
};

static int terminated(const char *value, size_t capacity) {
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static int valid_input(const h2_h2loader_host_job_input_t *input) {
    int image_valid;

    if (input == NULL) {
        return 0;
    }
    image_valid = input->asset.identity_source ==
            H2_H2LOADER_HOST_ASSET_IDENTITY_PACKAGE_MANIFEST
        ? input->asset.image[0] == '\0'
        : h2_h2loader_host_is_safe_identity(input->asset.image);
    return input != NULL &&
        terminated(input->fixture_slot, sizeof(input->fixture_slot)) &&
        terminated(
            input->candidate.endpoint,
            sizeof(input->candidate.endpoint)) &&
        terminated(
            input->candidate.candidate_id,
            sizeof(input->candidate.candidate_id)) &&
        terminated(
            input->candidate.usb_serial,
            sizeof(input->candidate.usb_serial)) &&
        terminated(input->asset.board, sizeof(input->asset.board)) &&
        terminated(input->asset.target, sizeof(input->asset.target)) &&
        terminated(input->asset.image, sizeof(input->asset.image)) &&
        terminated(input->asset.version, sizeof(input->asset.version)) &&
        terminated(input->asset.sha256, sizeof(input->asset.sha256)) &&
        terminated(
            input->asset.image_sha256,
            sizeof(input->asset.image_sha256)) &&
        input->candidate.usb_identity_valid <= 1u &&
        (input->candidate.usb_identity_valid == 0u ||
         input->candidate.usb_serial[0] != '\0') &&
        h2_h2loader_host_is_safe_identity(input->fixture_slot) &&
        input->candidate.transport >= H2_H2LOADER_HOST_TRANSPORT_SERIAL &&
        input->candidate.transport <= H2_H2LOADER_HOST_TRANSPORT_BLE &&
        input->candidate.endpoint[0] != '\0' &&
        h2_h2loader_host_is_safe_identity(input->asset.board) &&
        h2_h2loader_host_is_safe_identity(input->asset.target) &&
        image_valid &&
        h2_h2loader_host_is_safe_identity(input->asset.version) &&
        h2_h2loader_host_is_sha256(input->asset.sha256) &&
        h2_h2loader_host_is_sha256(input->asset.image_sha256) &&
        input->asset.role >= H2_H2LOADER_HOST_ASSET_ROLE_APP &&
        input->asset.role <= H2_H2LOADER_HOST_ASSET_ROLE_LOADER &&
        input->asset.operation >=
            H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL &&
        input->asset.operation <=
            H2_H2LOADER_HOST_ASSET_OPERATION_DIAGNOSTIC &&
        input->asset.identity_source >=
            H2_H2LOADER_HOST_ASSET_IDENTITY_RELEASE_CATALOG &&
        input->asset.identity_source <=
            H2_H2LOADER_HOST_ASSET_IDENTITY_PACKAGE_MANIFEST;
}

static int same_candidate_identity(
    const h2_h2loader_host_candidate_t *left,
    const h2_h2loader_host_candidate_t *right) {
    if (strcmp(left->candidate_id, right->candidate_id) == 0) {
        return 1;
    }
    if (left->transport != right->transport) {
        return 0;
    }
    if (left->transport == H2_H2LOADER_HOST_TRANSPORT_SERIAL) {
        return left->usb_identity_valid != 0u &&
            right->usb_identity_valid != 0u &&
            left->usb_vid == right->usb_vid &&
            left->usb_pid == right->usb_pid &&
            strcmp(left->usb_serial, right->usb_serial) == 0;
    }
    return left->ble_address.type == right->ble_address.type &&
        memcmp(
            left->ble_address.value,
            right->ble_address.value,
            H2_PAL_BLE_ADDR_LEN) == 0;
}

h2_pal_result_t h2_h2loader_host_scheduler_open(
    const h2_h2loader_host_scheduler_config_t *config,
    h2_h2loader_host_scheduler_t **out_scheduler) {
    if (out_scheduler != NULL) {
        *out_scheduler = NULL;
    }
    if (config == NULL || out_scheduler == NULL ||
        config->allocator == NULL || config->jobs == NULL ||
        config->job_count == 0u || config->max_concurrency == 0u ||
        config->max_concurrency > config->job_count ||
        config->job_count >
            SIZE_MAX / sizeof(h2_h2loader_host_job_result_t)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < config->job_count; ++i) {
        if (!valid_input(&config->jobs[i])) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        for (size_t j = 0u; j < i; ++j) {
            if (strcmp(
                    config->jobs[i].fixture_slot,
                    config->jobs[j].fixture_slot) == 0) {
                return H2_PAL_ERR_INVALID_ARG;
            }
            if (same_candidate_identity(
                    &config->jobs[i].candidate,
                    &config->jobs[j].candidate)) {
                return H2_PAL_ERR_INVALID_ARG;
            }
        }
    }
    h2_h2loader_host_scheduler_t *scheduler = h2_pal_mem_alloc(
        config->allocator, sizeof(*scheduler));
    if (scheduler == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->jobs = h2_pal_mem_alloc(
        config->allocator,
        config->job_count * sizeof(*scheduler->jobs));
    if (scheduler->jobs == NULL) {
        h2_pal_mem_free(config->allocator, scheduler);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(
        scheduler->jobs,
        0,
        config->job_count * sizeof(*scheduler->jobs));
    scheduler->allocator = config->allocator;
    scheduler->job_count = config->job_count;
    scheduler->max_concurrency = config->max_concurrency;
    for (size_t i = 0u; i < config->job_count; ++i) {
        scheduler->jobs[i].input = config->jobs[i];
        scheduler->jobs[i].state = H2_H2LOADER_HOST_JOB_QUEUED;
        scheduler->jobs[i].result = H2_PAL_ERR_INVALID_STATE;
    }
    *out_scheduler = scheduler;
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_scheduler_close(
    h2_h2loader_host_scheduler_t **inout_scheduler) {
    if (inout_scheduler == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_h2loader_host_scheduler_t *scheduler = *inout_scheduler;
    if (scheduler == NULL) {
        return H2_PAL_OK;
    }
    if (scheduler->active_count != 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const h2_pal_mem_api_t *allocator = scheduler->allocator;
    h2_pal_mem_free(allocator, scheduler->jobs);
    h2_pal_mem_free(allocator, scheduler);
    *inout_scheduler = NULL;
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_scheduler_claim(
    h2_h2loader_host_scheduler_t *scheduler,
    uint64_t now_ms,
    size_t *out_index,
    h2_h2loader_host_job_input_t *out_input) {
    if (scheduler == NULL || out_index == NULL || out_input == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (scheduler->paused) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (scheduler->active_count >= scheduler->max_concurrency) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    for (size_t i = 0u; i < scheduler->job_count; ++i) {
        h2_h2loader_host_job_result_t *job = &scheduler->jobs[i];
        if (job->state != H2_H2LOADER_HOST_JOB_QUEUED) {
            continue;
        }
        job->state = H2_H2LOADER_HOST_JOB_RUNNING;
        job->started_ms = now_ms;
        job->finished_ms = 0u;
        job->result = H2_PAL_ERR_INVALID_STATE;
        memset(&job->final_status, 0, sizeof(job->final_status));
        job->error_detail[0] = '\0';
        ++scheduler->active_count;
        *out_index = i;
        *out_input = job->input;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_FULL;
}

h2_pal_result_t h2_h2loader_host_scheduler_complete(
    h2_h2loader_host_scheduler_t *scheduler,
    size_t index,
    h2_pal_result_t result,
    const h2_h2loader_host_status_t *final_status,
    const char *error_detail,
    uint64_t now_ms) {
    if (scheduler == NULL || index >= scheduler->job_count ||
        scheduler->jobs[index].state != H2_H2LOADER_HOST_JOB_RUNNING ||
        now_ms < scheduler->jobs[index].started_ms ||
        (result == H2_PAL_OK && final_status == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (final_status != NULL &&
        (!terminated(
             final_status->active_checksum,
             sizeof(final_status->active_checksum)) ||
         !terminated(final_status->board, sizeof(final_status->board)) ||
         !terminated(final_status->target, sizeof(final_status->target)) ||
         !terminated(
             final_status->active_name,
             sizeof(final_status->active_name)) ||
         !terminated(
             final_status->active_version,
             sizeof(final_status->active_version)) ||
         !terminated(final_status->state, sizeof(final_status->state)))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_h2loader_host_job_result_t *job = &scheduler->jobs[index];
    if (error_detail != NULL &&
        !h2_h2loader_host_copy_text(
            job->error_detail,
            sizeof(job->error_detail),
            error_detail,
            strlen(error_detail))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (final_status != NULL) {
        job->final_status = *final_status;
    }
    job->result = result;
    job->state = result == H2_PAL_OK
        ? H2_H2LOADER_HOST_JOB_SUCCEEDED
        : (result == H2_PAL_EXIT
            ? H2_H2LOADER_HOST_JOB_CANCELLED
            : H2_H2LOADER_HOST_JOB_FAILED);
    job->finished_ms = now_ms;
    --scheduler->active_count;
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_scheduler_set_paused(
    h2_h2loader_host_scheduler_t *scheduler,
    int paused) {
    if (scheduler == NULL || (paused != 0 && paused != 1)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    scheduler->paused = paused;
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_scheduler_cancel_queued(
    h2_h2loader_host_scheduler_t *scheduler,
    uint64_t now_ms,
    size_t *out_cancelled) {
    if (scheduler == NULL || out_cancelled == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    size_t cancelled = 0u;
    for (size_t i = 0u; i < scheduler->job_count; ++i) {
        h2_h2loader_host_job_result_t *job = &scheduler->jobs[i];
        if (job->state == H2_H2LOADER_HOST_JOB_QUEUED) {
            job->state = H2_H2LOADER_HOST_JOB_CANCELLED;
            job->result = H2_PAL_EXIT;
            job->finished_ms = now_ms;
            ++cancelled;
        }
    }
    *out_cancelled = cancelled;
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_scheduler_retry(
    h2_h2loader_host_scheduler_t *scheduler,
    size_t index) {
    if (scheduler == NULL || index >= scheduler->job_count ||
        (scheduler->jobs[index].state != H2_H2LOADER_HOST_JOB_FAILED &&
         scheduler->jobs[index].state != H2_H2LOADER_HOST_JOB_CANCELLED) ||
        scheduler->jobs[index].retry_count == UINT32_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_h2loader_host_job_result_t *job = &scheduler->jobs[index];
    ++job->retry_count;
    job->state = H2_H2LOADER_HOST_JOB_QUEUED;
    job->result = H2_PAL_ERR_INVALID_STATE;
    job->started_ms = 0u;
    job->finished_ms = 0u;
    job->error_detail[0] = '\0';
    memset(&job->final_status, 0, sizeof(job->final_status));
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_scheduler_get(
    const h2_h2loader_host_scheduler_t *scheduler,
    size_t index,
    h2_h2loader_host_job_result_t *out_result) {
    if (scheduler == NULL || index >= scheduler->job_count ||
        out_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_result = scheduler->jobs[index];
    return H2_PAL_OK;
}

static h2_pal_result_t write_text(
    h2_h2loader_host_export_write_fn write,
    void *user,
    const char *text) {
    return write(
        user, (const uint8_t *)text, strlen(text));
}

static h2_pal_result_t write_json_string(
    h2_h2loader_host_export_write_fn write,
    void *user,
    const char *value) {
    h2_pal_result_t rc = write_text(write, user, "\"");
    for (const unsigned char *cursor = (const unsigned char *)value;
         rc == H2_PAL_OK && *cursor != '\0';
         ++cursor) {
        char escaped[7];
        if (*cursor == '"' || *cursor == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)*cursor;
            escaped[2] = '\0';
        } else if (*cursor < 0x20u) {
            (void)snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
        } else {
            escaped[0] = (char)*cursor;
            escaped[1] = '\0';
        }
        rc = write_text(write, user, escaped);
    }
    return rc == H2_PAL_OK ? write_text(write, user, "\"") : rc;
}

static const char *state_name(h2_h2loader_host_job_state_t state) {
    switch (state) {
        case H2_H2LOADER_HOST_JOB_QUEUED: return "queued";
        case H2_H2LOADER_HOST_JOB_RUNNING: return "running";
        case H2_H2LOADER_HOST_JOB_SUCCEEDED: return "succeeded";
        case H2_H2LOADER_HOST_JOB_FAILED: return "failed";
        case H2_H2LOADER_HOST_JOB_CANCELLED: return "cancelled";
    }
    return "invalid";
}

static const char *transport_name(h2_h2loader_host_transport_t transport) {
    switch (transport) {
        case H2_H2LOADER_HOST_TRANSPORT_SERIAL: return "serial";
        case H2_H2LOADER_HOST_TRANSPORT_BLE: return "ble";
    }
    return "invalid";
}

static const char *operation_name(
    h2_h2loader_host_asset_operation_t operation) {
    switch (operation) {
        case H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL:
            return "managed";
        case H2_H2LOADER_HOST_ASSET_OPERATION_RECOVERY:
            return "recovery";
        case H2_H2LOADER_HOST_ASSET_OPERATION_DIAGNOSTIC:
            return "diagnostic";
    }
    return "invalid";
}

static const char *role_name(uint8_t role) {
    switch (role) {
        case H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER: return "loader";
        case H2_H2LOADER_HOST_ACTIVE_ROLE_APP: return "app";
        default: return "unknown";
    }
}

h2_pal_result_t h2_h2loader_host_scheduler_export_json(
    const h2_h2loader_host_scheduler_t *scheduler,
    h2_h2loader_host_export_write_fn write,
    void *write_user) {
    if (scheduler == NULL || write == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = write_text(
        write,
        write_user,
        "{\"format\":2,\"catalog_format\":1,\"jobs\":[");
    for (size_t i = 0u; rc == H2_PAL_OK && i < scheduler->job_count; ++i) {
        const h2_h2loader_host_job_result_t *job = &scheduler->jobs[i];
        char numbers[256];
        if (i != 0u) {
            rc = write_text(write, write_user, ",");
        }
#define H2_JSON_FIELD(name, value) \
        if (rc == H2_PAL_OK) { \
            rc = write_text(write, write_user, "\"" name "\":"); \
        } \
        if (rc == H2_PAL_OK) { \
            rc = write_json_string(write, write_user, value); \
        }
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, "{");
        H2_JSON_FIELD("fixture_slot", job->input.fixture_slot)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD(
            "transport", transport_name(job->input.candidate.transport))
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("endpoint", job->input.candidate.endpoint)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("candidate_id", job->input.candidate.candidate_id)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("usb_serial", job->input.candidate.usb_serial)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("board", job->input.asset.board)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("target", job->input.asset.target)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD(
            "operation", operation_name(job->input.asset.operation))
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("asset_name", job->input.asset.image)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("asset_version", job->input.asset.version)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("asset_sha256", job->input.asset.sha256)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD(
            "asset_image_sha256", job->input.asset.image_sha256)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("state", state_name(job->state))
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("final_board", job->final_status.board)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("final_target", job->final_status.target)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD(
            "final_role", role_name(job->final_status.active_role))
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("final_name", job->final_status.active_name)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("final_version", job->final_status.active_version)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("final_state", job->final_status.state)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("final_checksum", job->final_status.active_checksum)
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, ",");
        H2_JSON_FIELD("error_detail", job->error_detail)
#undef H2_JSON_FIELD
        int count = snprintf(
            numbers,
            sizeof(numbers),
            ",\"result\":%d,\"started_ms\":%llu,\"finished_ms\":%llu,"
            "\"retry_count\":%u,\"usb_vid\":%u,\"usb_pid\":%u,"
            "\"staged_valid\":%u}",
            (int)job->result,
            (unsigned long long)job->started_ms,
            (unsigned long long)job->finished_ms,
            job->retry_count,
            (unsigned)job->input.candidate.usb_vid,
            (unsigned)job->input.candidate.usb_pid,
            (unsigned)job->final_status.staged_valid);
        if (count < 0 || (size_t)count >= sizeof(numbers)) {
            return H2_PAL_ERR_NO_SPACE;
        }
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, numbers);
    }
    return rc == H2_PAL_OK
        ? write_text(write, write_user, "]}\n")
        : rc;
}

static h2_pal_result_t write_csv_field(
    h2_h2loader_host_export_write_fn write,
    void *user,
    const char *value) {
    h2_pal_result_t rc = write_text(write, user, "\"");
    if (rc == H2_PAL_OK &&
        (value[0] == '=' || value[0] == '+' ||
         value[0] == '-' || value[0] == '@')) {
        rc = write_text(write, user, "'");
    }
    for (const char *cursor = value;
         rc == H2_PAL_OK && *cursor != '\0';
         ++cursor) {
        char bytes[3] = { *cursor, '\0', '\0' };
        if (*cursor == '"') {
            bytes[1] = '"';
        }
        rc = write_text(write, user, bytes);
    }
    return rc == H2_PAL_OK ? write_text(write, user, "\"") : rc;
}

h2_pal_result_t h2_h2loader_host_scheduler_export_csv(
    const h2_h2loader_host_scheduler_t *scheduler,
    h2_h2loader_host_export_write_fn write,
    void *write_user) {
    if (scheduler == NULL || write == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = write_text(
        write,
        write_user,
        "catalog_format,fixture_slot,transport,endpoint,candidate_id,"
        "usb_vid,usb_pid,usb_serial,board,target,operation,asset_name,"
        "asset_version,asset_sha256,asset_image_sha256,state,result,"
        "started_ms,finished_ms,retry_count,final_board,final_target,"
        "final_role,final_name,final_version,final_state,final_checksum,"
        "staged_valid,error_detail\r\n");
    for (size_t i = 0u; rc == H2_PAL_OK && i < scheduler->job_count; ++i) {
        const h2_h2loader_host_job_result_t *job = &scheduler->jobs[i];
        const char *fields[] = {
            job->input.fixture_slot,
            transport_name(job->input.candidate.transport),
            job->input.candidate.endpoint,
            job->input.candidate.candidate_id,
        };
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, "1,");
        for (size_t field = 0u;
             rc == H2_PAL_OK && field < sizeof(fields) / sizeof(fields[0]);
             ++field) {
            if (field != 0u) rc = write_text(write, write_user, ",");
            if (rc == H2_PAL_OK) {
                rc = write_csv_field(write, write_user, fields[field]);
            }
        }
        char identity_numbers[64];
        int identity_count = snprintf(
            identity_numbers,
            sizeof(identity_numbers),
            ",%u,%u,",
            (unsigned)job->input.candidate.usb_vid,
            (unsigned)job->input.candidate.usb_pid);
        if (identity_count < 0 ||
            (size_t)identity_count >= sizeof(identity_numbers)) {
            return H2_PAL_ERR_NO_SPACE;
        }
        if (rc == H2_PAL_OK) {
            rc = write_text(write, write_user, identity_numbers);
        }
        if (rc == H2_PAL_OK) {
            rc = write_csv_field(
                write, write_user, job->input.candidate.usb_serial);
        }
        const char *asset_fields[] = {
            job->input.asset.board,
            job->input.asset.target,
            operation_name(job->input.asset.operation),
            job->input.asset.image,
            job->input.asset.version,
            job->input.asset.sha256,
            job->input.asset.image_sha256,
            state_name(job->state),
        };
        for (size_t field = 0u;
             rc == H2_PAL_OK &&
             field < sizeof(asset_fields) / sizeof(asset_fields[0]);
             ++field) {
            rc = write_text(write, write_user, ",");
            if (rc == H2_PAL_OK) {
                rc = write_csv_field(
                    write, write_user, asset_fields[field]);
            }
        }
        char numbers[256];
        int count = snprintf(
            numbers,
            sizeof(numbers),
            ",%d,%llu,%llu,%u,",
            (int)job->result,
            (unsigned long long)job->started_ms,
            (unsigned long long)job->finished_ms,
            job->retry_count);
        if (count < 0 || (size_t)count >= sizeof(numbers)) {
            return H2_PAL_ERR_NO_SPACE;
        }
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, numbers);
        const char *final_fields[] = {
            job->final_status.board,
            job->final_status.target,
            role_name(job->final_status.active_role),
            job->final_status.active_name,
            job->final_status.active_version,
            job->final_status.state,
            job->final_status.active_checksum,
        };
        for (size_t field = 0u;
             rc == H2_PAL_OK &&
             field < sizeof(final_fields) / sizeof(final_fields[0]);
             ++field) {
            if (field != 0u) rc = write_text(write, write_user, ",");
            if (rc == H2_PAL_OK) {
                rc = write_csv_field(
                    write, write_user, final_fields[field]);
            }
        }
        char staged[16];
        int staged_count = snprintf(
            staged,
            sizeof(staged),
            ",%u,",
            (unsigned)job->final_status.staged_valid);
        if (staged_count < 0 || (size_t)staged_count >= sizeof(staged)) {
            return H2_PAL_ERR_NO_SPACE;
        }
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, staged);
        if (rc == H2_PAL_OK) {
            rc = write_csv_field(write, write_user, job->error_detail);
        }
        if (rc == H2_PAL_OK) rc = write_text(write, write_user, "\r\n");
    }
    return rc;
}
