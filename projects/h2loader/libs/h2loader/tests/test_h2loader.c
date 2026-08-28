#include "h2_loader_app_client.h"
#include "h2_loader_boot.h"
#include "h2_loader_ble.h"
#include "h2_loader_command.h"
#include "h2_loader_package.h"
#include "h2_loader_status.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define TEST_AB_SHA256 \
    "abababababababababababababababababababababababababababababababab"
#define TEST_CD_SHA256 \
    "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"

struct h2_pal_fs_file {
    size_t offset;
};

struct h2_pal_task {
    int completed;
};

typedef struct checksum_test_context {
    const uint8_t *data;
    size_t len;
    struct h2_pal_fs_file file;
    int closed;
    int aborted;
} checksum_test_context_t;

typedef struct validation_digest_context {
    uint8_t data[128];
    size_t len;
    int aborts;
} validation_digest_context_t;

typedef struct idle_trial_context {
    h2_pal_pref_namespace_t pref_namespace;
    uint8_t upgrade_record[384];
    size_t upgrade_record_len;
    char upgrade_step[H2_LOADER_IDENTITY_TEXT_MAX];
    int upgrade_step_present;
    int upgrade_step_removes;
    uint32_t running_partition;
    uint32_t selected_partition;
    int boot_selections;
    int hold_calls;
    int hold_value;
    int hold_result;
    int confirms;
    int reboots;
    int reboot_transitions;
    int reboots_at_transition;
    int reboot_transition_result;
    int reboot_result;
    int disruptive_calls;
    h2_loader_disruptive_action_t disruptive_action;
    int disruptive_result;
    int32_t last_result;
    int last_result_sets;
    int commits;
    int commit_result;
    int confirm_result;
    h2_loader_boot_intent_t boot_intent;
    uint32_t install_state;
    int app_confirmed;
    int manual_hold;
    int installed_valid;
    char installed_version[H2_LOADER_IDENTITY_TEXT_MAX];
    char installed_checksum[H2_LOADER_IDENTITY_TEXT_MAX];
    uint32_t installed_size;
    h2_loader_boot_intent_t pending_boot_intent;
    uint32_t pending_install_state;
    int pending_app_confirmed;
    int pending_manual_hold;
    int pending_lifecycle;
    int lifecycle_set_calls;
    int lifecycle_set_fail_after;
    int lifecycle_set_result;
    int sequence;
    int commit_sequence;
    int selection_sequence;
    int transition_sequence;
    int disruptive_sequence;
    int reboot_sequence;
} idle_trial_context_t;

typedef struct command_test_io {
    const char *input;
    size_t input_len;
    size_t input_offset;
    char output[512];
    size_t output_len;
    size_t writes;
    size_t flushes;
    h2_pal_result_t exhausted_result;
} command_test_io_t;

typedef struct capability_lock_test_context {
    h2_loader_t *loader;
    uint32_t command;
    size_t lock_calls;
    size_t unlock_calls;
} capability_lock_test_context_t;

static h2_pal_result_t capability_lock_test_lock(
    void *user,
    h2_pal_mutex_t *mutex) {
    capability_lock_test_context_t *context = user;

    (void)mutex;
    context->lock_calls += 1u;
    return (h2_pal_result_t)h2_loader_set_command_availability(
        context->loader, context->command, false);
}

static h2_pal_result_t capability_lock_test_unlock(
    void *user,
    h2_pal_mutex_t *mutex) {
    capability_lock_test_context_t *context = user;

    (void)mutex;
    context->unlock_calls += 1u;
    return H2_PAL_OK;
}

typedef struct wifi_command_test_context {
    h2_pal_wifi_sta_config_t connected;
    h2_pal_wifi_sta_config_t saved;
    h2_pal_result_t save_result;
    h2_pal_result_t scan_result;
    uint32_t scan_timeout_ms;
    int disconnect_after_save;
    int connect_calls;
    int status_calls;
    int save_calls;
    int scan_calls;
} wifi_command_test_context_t;

typedef struct stage_test_context {
    struct h2_pal_fs_file file;
    h2_pal_pref_namespace_t pref_namespace;
    int package_exists;
    int temporary_exists;
    int previous_exists;
    int package_is_dir;
    int temporary_is_dir;
    int previous_is_dir;
    uint64_t package_size;
    uint64_t temporary_size;
    uint64_t previous_size;
    uint64_t capacity;
    size_t opens;
    size_t closes;
    size_t removes;
    size_t writes;
    int package_present_when_opened;
    int package_present_when_wifi_checked;
    h2_loader_boot_intent_t boot_intent;
    h2_loader_install_state_t install_state;
    int app_confirmed;
    int manual_hold;
    int32_t last_result;
    int installed_valid;
    char installed_version[H2_LOADER_IDENTITY_TEXT_MAX];
    char installed_checksum[H2_LOADER_IDENTITY_TEXT_MAX];
    uint32_t installed_size;
    int staged_valid;
    char staged_version[H2_LOADER_IDENTITY_TEXT_MAX];
    char staged_checksum[H2_LOADER_IDENTITY_TEXT_MAX];
    uint32_t staged_size;
    int publish_marker_present;
    int publish_committed;
    int pref_set_error;
    int pref_commit_error;
    size_t pref_commits;
    uint32_t running_partition;
    uint32_t next_partition;
    size_t wifi_status_calls;
    uint8_t upgrade_record[384];
    size_t upgrade_record_len;
} stage_test_context_t;

typedef struct stage_test_fixture {
    stage_test_context_t context;
    h2_pal_fs_api_t fs;
    h2_pal_pref_api_t pref;
    h2_pal_power_api_t power;
    h2_pal_mem_api_t mem;
    h2_pal_http_api_t http;
    h2_pal_wifi_sta_api_t wifi;
    h2_pal_disk_api_t disk;
    h2_loader_t loader;
} stage_test_fixture_t;

static h2_pal_result_t command_test_read(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    command_test_io_t *io = (command_test_io_t *)user;
    size_t available = io->input_len - io->input_offset;
    size_t take = len < available ? len : available;
    (void)timeout_ms;

    *out_read = take;
    if (take == 0u) {
        return io->exhausted_result == H2_PAL_OK ?
            H2_PAL_ERR_TIMEOUT : io->exhausted_result;
    }
    memcpy(buffer, io->input + io->input_offset, take);
    io->input_offset += take;
    return H2_PAL_OK;
}

static h2_pal_result_t command_test_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
    command_test_io_t *io = (command_test_io_t *)user;
    (void)timeout_ms;

    assert(io->output_len + len <= sizeof(io->output));
    memcpy(io->output + io->output_len, buffer, len);
    io->output_len += len;
    io->writes += 1u;
    *out_written = len;
    return H2_PAL_OK;
}

static h2_pal_result_t command_test_flush(void *user) {
    command_test_io_t *io = (command_test_io_t *)user;
    io->flushes += 1u;
    return H2_PAL_OK;
}

static int command_test_digest_start(void *user) {
    (void)user;
    return H2_PAL_OK;
}

static int command_test_digest_update(void *user, const uint8_t *data, size_t len) {
    (void)user;
    (void)data;
    (void)len;
    return H2_PAL_OK;
}

static int command_test_digest_finish(void *user, uint8_t out_digest[32]) {
    (void)user;
    memset(out_digest, 0, 32u);
    return H2_PAL_OK;
}

static uint64_t command_test_now_ms(void *user) {
    (void)user;
    return 0u;
}

static void command_test_sleep_ms(void *user, uint32_t delay_ms) {
    (void)user;
    (void)delay_ms;
}

static int wifi_command_test_connect(
    void *user,
    const h2_pal_wifi_sta_config_t *config,
    uint32_t timeout_ms) {
    wifi_command_test_context_t *context = user;

    assert(timeout_ms != 0u);
    context->connected = *config;
    context->connect_calls++;
    return H2_PAL_OK;
}

static int wifi_command_test_scan(
    void *user,
    const h2_pal_wifi_scan_request_t *request,
    h2_pal_wifi_scan_result_fn on_result,
    void *callback_user,
    uint32_t timeout_ms) {
    static const h2_pal_wifi_scan_entry_t entries[] = {
        {
            .ssid = "Cafe WiFi",
            .ssid_len = 9u,
            .bssid = {0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u},
            .channel = 6u,
            .rssi = -42,
            .security = H2_PAL_WIFI_SECURITY_WPA2,
        },
        {
            .ssid = "Lab",
            .ssid_len = 3u,
            .bssid = {0xaau, 0xbbu, 0xccu, 0xddu, 0xeeu, 0xffu},
            .channel = 11u,
            .rssi = -55,
            .security = H2_PAL_WIFI_SECURITY_OPEN,
        },
        {
            .ssid_len = 0u,
            .channel = 1u,
            .rssi = -70,
            .security = H2_PAL_WIFI_SECURITY_UNKNOWN,
        },
    };
    wifi_command_test_context_t *context = user;

    assert(request == NULL);
    assert(on_result != NULL);
    context->scan_calls++;
    context->scan_timeout_ms = timeout_ms;
    for (size_t i = 0u; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        if (!on_result(callback_user, &entries[i])) break;
    }
    return context->scan_result;
}

static int wifi_command_test_get_status(
    void *user,
    h2_pal_wifi_sta_status_t *out_status) {
    wifi_command_test_context_t *context = user;

    memset(out_status, 0, sizeof(*out_status));
    if (context->disconnect_after_save != 0 &&
        context->save_calls != 0 &&
        context->connect_calls == 1) {
        out_status->state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
    } else {
        out_status->state = H2_PAL_WIFI_STA_STATE_GOT_IP;
        out_status->ip_valid = 1u;
    }
    context->status_calls++;
    return H2_PAL_OK;
}

static int wifi_command_test_save(
    void *user,
    const h2_pal_wifi_sta_config_t *config) {
    wifi_command_test_context_t *context = user;

    assert(context->connect_calls == 1);
    context->saved = *config;
    context->save_calls++;
    return context->save_result;
}

static void test_wifi_connect_persists_confirmed_config(void) {
    static const char *const argv[] = {
        "h2loader", "wifi", "connect", "test-network", "test-password"};
    static const h2_command_io_vtable_t io_vtable = {
        .read = command_test_read,
        .write = command_test_write,
        .flush = command_test_flush,
    };
    static const h2_pal_fs_vtable_t fs_vtable = {0};
    static const h2_pal_http_vtable_t http_vtable = {0};
    static const h2_pal_wifi_sta_vtable_t wifi_vtable = {
        .get_status = wifi_command_test_get_status,
        .connect = wifi_command_test_connect,
    };
    static const h2_pal_wifi_settings_vtable_t wifi_settings_vtable = {
        .set_saved_sta_config = wifi_command_test_save,
    };
    static const h2_pal_disk_vtable_t disk_vtable = {0};
    command_test_io_t io = {0};
    wifi_command_test_context_t context = {
        .disconnect_after_save = 1,
    };
    h2_loader_t loader = {0};
    h2_loader_command_t command;
    h2_pal_fs_api_t fs = {.vtable = &fs_vtable};
    h2_pal_http_api_t http = {.vtable = &http_vtable};
    h2_pal_wifi_sta_api_t wifi = {
        .user = &context,
        .vtable = &wifi_vtable,
    };
    h2_pal_wifi_settings_api_t wifi_settings = {
        .user = &context,
        .vtable = &wifi_settings_vtable,
    };
    h2_pal_disk_api_t disk = {.vtable = &disk_vtable};
    h2_loader_command_config_t config = {
        .loader = &loader,
        .fs = &fs,
        .http = &http,
        .wifi = &wifi,
        .wifi_settings = &wifi_settings,
        .disk = &disk,
        .digest = {
            .start = command_test_digest_start,
            .update = command_test_digest_update,
            .finish = command_test_digest_finish,
        },
        .now_ms = command_test_now_ms,
        .sleep_ms = command_test_sleep_ms,
        .io = {
            .user = &io,
            .vtable = &io_vtable,
        },
    };

    assert(h2_loader_command_init(&command, &config) == H2_PAL_OK);
    assert(h2_loader_command_execute(
               &command, sizeof(argv) / sizeof(argv[0]), argv) == H2_PAL_OK);
    assert(context.connect_calls == 2);
    assert(context.status_calls == 4);
    assert(context.save_calls == 1);
    assert(context.connected.ssid_len == strlen("test-network"));
    assert(context.saved.ssid_len == context.connected.ssid_len);
    assert(memcmp(
               context.saved.ssid,
               context.connected.ssid,
               context.saved.ssid_len) == 0);
    assert(context.saved.password_len == context.connected.password_len);
    assert(memcmp(
               context.saved.password,
               context.connected.password,
               context.saved.password_len) == 0);

    memset(&context, 0, sizeof(context));
    context.save_result = H2_PAL_ERR_IO;
    assert(h2_loader_command_init(&command, &config) == H2_PAL_OK);
    assert(h2_loader_command_execute(
               &command, sizeof(argv) / sizeof(argv[0]), argv) ==
           H2_PAL_ERR_IO);
    assert(context.connect_calls == 1);
    assert(context.status_calls == 2);
    assert(context.save_calls == 1);
}

static void test_wifi_scan_is_bounded_and_structured(void) {
    static const char *const defaults[] = {
        "h2loader", "wifi", "scan"};
    static const char *const argv[] = {
        "h2loader", "wifi", "scan", "--timeout-ms", "2500",
        "--limit", "2"};
    static const char *const invalid[] = {
        "h2loader", "wifi", "scan", "--limit", "0"};
    static const h2_command_io_vtable_t io_vtable = {
        .read = command_test_read,
        .write = command_test_write,
        .flush = command_test_flush,
    };
    static const h2_pal_fs_vtable_t fs_vtable = {0};
    static const h2_pal_http_vtable_t http_vtable = {0};
    static const h2_pal_wifi_sta_vtable_t wifi_vtable = {
        .scan = wifi_command_test_scan,
    };
    static const h2_pal_disk_vtable_t disk_vtable = {0};
    command_test_io_t io = {0};
    wifi_command_test_context_t context = {0};
    h2_loader_t loader = {0};
    h2_loader_command_t command;
    h2_pal_fs_api_t fs = {.vtable = &fs_vtable};
    h2_pal_http_api_t http = {.vtable = &http_vtable};
    h2_pal_wifi_sta_api_t wifi = {
        .user = &context,
        .vtable = &wifi_vtable,
    };
    h2_pal_disk_api_t disk = {.vtable = &disk_vtable};
    h2_loader_command_config_t config = {
        .loader = &loader,
        .fs = &fs,
        .http = &http,
        .wifi = &wifi,
        .disk = &disk,
        .digest = {
            .start = command_test_digest_start,
            .update = command_test_digest_update,
            .finish = command_test_digest_finish,
        },
        .now_ms = command_test_now_ms,
        .sleep_ms = command_test_sleep_ms,
        .io = {
            .user = &io,
            .vtable = &io_vtable,
        },
    };

    assert(h2_loader_command_init(&command, &config) == H2_PAL_OK);
    assert(h2_loader_command_execute(
               &command, sizeof(argv) / sizeof(argv[0]), argv) == H2_PAL_OK);
    assert(context.scan_calls == 1);
    assert(context.scan_timeout_ms == 2500u);
    assert(strstr(io.output,
        "H2_LOADER_WIFI_SCAN_RESULT index=1 ssid_hex=436166652057694669 ") != NULL);
    assert(strstr(io.output,
        "bssid=001122334455 channel=6 rssi=-42 security=4\n") != NULL);
    assert(strstr(io.output,
        "H2_LOADER_WIFI_SCAN_RESULT index=2 ssid_hex=4c6162 ") != NULL);
    assert(strstr(io.output,
        "H2_LOADER_WIFI_SCAN_DONE result=OK code=0 count=2\n") != NULL);
    assert(strstr(io.output, "index=3") == NULL);

    memset(&io, 0, sizeof(io));
    assert(h2_loader_command_init(&command, &config) == H2_PAL_OK);
    assert(h2_loader_command_execute(
               &command, sizeof(defaults) / sizeof(defaults[0]), defaults) ==
           H2_PAL_OK);
    assert(context.scan_calls == 2);
    assert(context.scan_timeout_ms == 10000u);
    assert(strstr(io.output,
        "H2_LOADER_WIFI_SCAN_RESULT index=3 ssid_hex= ") != NULL);
    assert(strstr(io.output,
        "H2_LOADER_WIFI_SCAN_DONE result=OK code=0 count=3\n") != NULL);

    memset(&io, 0, sizeof(io));
    context.scan_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_loader_command_init(&command, &config) == H2_PAL_OK);
    assert(h2_loader_command_execute(
               &command, sizeof(argv) / sizeof(argv[0]), argv) ==
           H2_PAL_ERR_TIMEOUT);
    assert(context.scan_calls == 3);
    assert(strstr(io.output,
        "H2_LOADER_WIFI_SCAN_DONE result=error code=") != NULL);
    context.scan_result = H2_PAL_OK;

    memset(&io, 0, sizeof(io));
    assert(h2_loader_command_init(&command, &config) == H2_PAL_OK);
    assert(h2_loader_command_execute(
               &command, sizeof(invalid) / sizeof(invalid[0]), invalid) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(context.scan_calls == 3);
}

static h2_pal_result_t command_test_memory_read(
    void *user,
    h2_loader_memory_stats_t *out_stats) {
    (void)user;
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->internal.total_bytes = 1000u;
    out_stats->internal.free_bytes = 600u;
    out_stats->internal.minimum_free_bytes = 500u;
    out_stats->internal.largest_free_block_bytes = 400u;
    out_stats->iram.total_bytes = 300u;
    out_stats->iram.free_bytes = 200u;
    out_stats->iram.minimum_free_bytes = 150u;
    out_stats->iram.largest_free_block_bytes = 100u;
    out_stats->psram.total_bytes = 8000u;
    out_stats->psram.free_bytes = 7000u;
    out_stats->psram.minimum_free_bytes = 6500u;
    out_stats->psram.largest_free_block_bytes = 6000u;
    return H2_PAL_OK;
}

static h2_pal_result_t command_test_init(
    h2_loader_command_t *command,
    h2_loader_t *loader,
    command_test_io_t *io) {
    static const h2_command_io_vtable_t io_vtable = {
        .read = command_test_read,
        .write = command_test_write,
        .flush = command_test_flush,
    };
    static const h2_pal_fs_vtable_t fs_vtable = {0};
    static const h2_pal_http_vtable_t http_vtable = {0};
    static const h2_pal_wifi_sta_vtable_t wifi_vtable = {0};
    static const h2_pal_disk_vtable_t disk_vtable = {0};
    static const h2_pal_fs_api_t fs = {.vtable = &fs_vtable};
    static const h2_pal_http_api_t http = {.vtable = &http_vtable};
    static const h2_pal_wifi_sta_api_t wifi = {.vtable = &wifi_vtable};
    static const h2_pal_disk_api_t disk = {.vtable = &disk_vtable};
    h2_loader_command_config_t config;

    memset(&config, 0, sizeof(config));
    config.loader = loader;
    config.fs = &fs;
    config.http = &http;
    config.wifi = &wifi;
    config.disk = &disk;
    config.digest.start = command_test_digest_start;
    config.digest.update = command_test_digest_update;
    config.digest.finish = command_test_digest_finish;
    config.memory_stats.read = command_test_memory_read;
    config.now_ms = command_test_now_ms;
    config.sleep_ms = command_test_sleep_ms;
    config.io.user = io;
    config.io.vtable = &io_vtable;
    return (h2_pal_result_t)h2_loader_command_init(command, &config);
}

typedef struct mfg_pref_context {
    h2_pal_pref_namespace_t pref_namespace;
    uint8_t persisted[64];
    uint8_t pending[64];
    size_t persisted_len;
    size_t pending_len;
    uint32_t persisted_revision;
    uint32_t pending_revision;
    int revision_present;
    int pending_revision_set;
    int commit_result;
    int commits;
} mfg_pref_context_t;

typedef struct disk_progress_context {
    uint8_t source[1536];
    uint8_t destination[1536];
    uint64_t last_completed;
    uint64_t last_total;
    int image_progress_calls;
    size_t max_read_len;
    size_t max_write_len;
} disk_progress_context_t;

typedef struct install_plan_context {
    struct h2_pal_fs_file checksum_file;
    const uint8_t *checksum;
    size_t checksum_len;
    uint8_t app_digest_byte;
    int checksum_present;
    int checksum_open_error;
    int disk_read_error;
    int checksum_closes;
} install_plan_context_t;

static void *test_mem_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void test_mem_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static int install_plan_fs_open(
    void *user,
    const char *path,
    h2_pal_fs_open_mode_t mode,
    h2_pal_fs_file_t **out_file) {
    install_plan_context_t *context = (install_plan_context_t *)user;

    assert(strcmp(path, H2_LOADER_DEFAULT_CHECKSUM_PATH) == 0);
    assert(mode == H2_PAL_FS_OPEN_READ);
    if (context->checksum_open_error != H2_PAL_OK) {
        return context->checksum_open_error;
    }
    if (!context->checksum_present) {
        return H2_PAL_FS_ERR_NOT_FOUND;
    }
    context->checksum_file.offset = 0u;
    *out_file = &context->checksum_file;
    return H2_PAL_FS_OK;
}

static int install_plan_fs_read(
    void *user,
    h2_pal_fs_file_t *file,
    void *data,
    size_t len,
    size_t *out_read) {
    install_plan_context_t *context = (install_plan_context_t *)user;
    size_t remaining = context->checksum_len - file->offset;
    size_t take = remaining < len ? remaining : len;

    memcpy(data, context->checksum + file->offset, take);
    file->offset += take;
    *out_read = take;
    return H2_PAL_FS_OK;
}

static int install_plan_fs_close(void *user, h2_pal_fs_file_t *file) {
    install_plan_context_t *context = (install_plan_context_t *)user;
    (void)file;
    context->checksum_closes += 1;
    return H2_PAL_FS_OK;
}

static h2_pal_result_t install_plan_disk_get_partition(
    void *user,
    uint32_t partition_id,
    h2_pal_disk_partition_t *out_partition) {
    (void)user;
    assert(partition_id == 7u);
    memset(out_partition, 0, sizeof(*out_partition));
    out_partition->id = partition_id;
    out_partition->size = 4096u;
    return H2_PAL_OK;
}

static h2_pal_result_t install_plan_disk_read(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    void *data,
    size_t len) {
    install_plan_context_t *context = (install_plan_context_t *)user;
    assert(partition_id == 7u);
    assert(offset == 0u);
    if (context->disk_read_error != H2_PAL_OK) {
        return context->disk_read_error;
    }
    memset(data, 0x5au, len);
    return H2_PAL_OK;
}

static int install_plan_digest_start(void *user) {
    (void)user;
    return H2_PAL_OK;
}

static int install_plan_digest_update(void *user, const uint8_t *data, size_t len) {
    (void)user;
    (void)data;
    (void)len;
    return H2_PAL_OK;
}

static int install_plan_digest_finish(void *user, uint8_t out_digest[32]) {
    install_plan_context_t *context = (install_plan_context_t *)user;
    memset(out_digest, context->app_digest_byte, 32u);
    return H2_PAL_OK;
}

static void install_plan_digest_abort(void *user) {
    (void)user;
}

static void fill_sha256(char out[H2_LOADER_SHA256_HEX_SIZE], char pair_digit) {
    memset(out, pair_digit, H2_LOADER_SHA256_HEX_SIZE - 1u);
    out[H2_LOADER_SHA256_HEX_SIZE - 1u] = '\0';
}

static void test_install_plan_selects_changed_components_independently(void) {
    static const uint8_t package_checksum[] =
        "abababababababababababababababababababababababababababababababab\n";
    static const uint8_t different_checksum[] =
        "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd\n";
    install_plan_context_t context = {0};
    h2_loader_package_t package = {0};
    h2_loader_package_inspection_t inspection = {0};
    h2_loader_package_install_plan_t plan;
    const h2_pal_fs_vtable_t fs_vtable = {
        .open = install_plan_fs_open,
        .read = install_plan_fs_read,
        .close = install_plan_fs_close,
    };
    const h2_pal_disk_vtable_t disk_vtable = {
        .get_partition = install_plan_disk_get_partition,
        .read = install_plan_disk_read,
    };
    const h2_pal_fs_api_t fs = {.user = &context, .vtable = &fs_vtable};
    const h2_pal_disk_api_t disk = {.user = &context, .vtable = &disk_vtable};

    package.config.fs = &fs;
    package.config.disk = &disk;
    package.config.installed_checksum_path = H2_LOADER_DEFAULT_CHECKSUM_PATH;
    package.config.digest = (h2_loader_digest_api_t){
        .user = &context,
        .start = install_plan_digest_start,
        .update = install_plan_digest_update,
        .finish = install_plan_digest_finish,
        .abort = install_plan_digest_abort,
    };
    inspection.manifest.role = H2_LOADER_IMAGE_ROLE_APP;
    inspection.manifest.image_size = 32u;
    fill_sha256(inspection.manifest.image_sha256, 'a');
    memcpy(inspection.data_checksum, package_checksum, sizeof(package_checksum) - 1u);
    inspection.data_checksum_len = sizeof(package_checksum) - 1u;

    context.app_digest_byte = 0xaau;
    context.checksum = package_checksum;
    context.checksum_len = sizeof(package_checksum) - 1u;
    context.checksum_present = 1;
    assert(h2_loader_package_plan_install(&package, &inspection, 7u, &plan) == H2_PAL_OK);
    assert(plan.update_app == 0 && plan.update_data == 0);

    context.app_digest_byte = 0xbbu;
    assert(h2_loader_package_plan_install(&package, &inspection, 7u, &plan) == H2_PAL_OK);
    assert(plan.update_app == 1 && plan.update_data == 0);

    context.app_digest_byte = 0xaau;
    context.checksum = different_checksum;
    context.checksum_len = sizeof(different_checksum) - 1u;
    assert(h2_loader_package_plan_install(&package, &inspection, 7u, &plan) == H2_PAL_OK);
    assert(plan.update_app == 0 && plan.update_data == 1);

    context.app_digest_byte = 0xbbu;
    context.checksum_present = 0;
    assert(h2_loader_package_plan_install(&package, &inspection, 7u, &plan) == H2_PAL_OK);
    assert(plan.update_app == 1 && plan.update_data == 1);
    assert(context.checksum_closes == 3);

    inspection.manifest.role = H2_LOADER_IMAGE_ROLE_H2LOADER;
    assert(h2_loader_package_plan_install(&package, &inspection, 7u, &plan) == H2_PAL_OK);
    assert(plan.update_app == 1 && plan.update_data == 0);

    inspection.manifest.role = H2_LOADER_IMAGE_ROLE_APP;
    context.checksum_present = 1;
    context.checksum = package_checksum;
    context.checksum_len = sizeof(package_checksum) - 1u;
    context.disk_read_error = H2_PAL_ERR_IO;
    assert(h2_loader_package_plan_install(&package, &inspection, 7u, &plan) ==
        H2_PAL_ERR_IO);
    context.disk_read_error = H2_PAL_OK;
    context.checksum_open_error = H2_PAL_ERR_IO;
    assert(h2_loader_package_plan_install(&package, &inspection, 7u, &plan) ==
        H2_PAL_ERR_IO);
}

static int mfg_pref_close(h2_pal_pref_namespace_t *ns) {
    (void)ns;
    return H2_PAL_OK;
}

static int mfg_pref_get_blob(
    h2_pal_pref_namespace_t *ns,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    void **out_data,
    size_t *out_len) {
    mfg_pref_context_t *context = (mfg_pref_context_t *)ns->user;
    assert(strcmp(key, "mfg") == 0);
    if (context->persisted_len == 0u) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_data = h2_pal_mem_alloc(allocator, context->persisted_len);
    assert(*out_data != NULL);
    memcpy(*out_data, context->persisted, context->persisted_len);
    *out_len = context->persisted_len;
    return H2_PAL_OK;
}

static int mfg_pref_set_blob(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    const void *data,
    size_t len) {
    mfg_pref_context_t *context = (mfg_pref_context_t *)ns->user;
    assert(strcmp(key, "mfg") == 0);
    assert(len <= sizeof(context->pending));
    memcpy(context->pending, data, len);
    context->pending_len = len;
    return H2_PAL_OK;
}

static int mfg_pref_get_u32(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    uint32_t *out_value) {
    mfg_pref_context_t *context = (mfg_pref_context_t *)ns->user;
    assert(strcmp(key, "mfg_acceptance_revision") == 0);
    if (!context->revision_present) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_value = context->persisted_revision;
    return H2_PAL_OK;
}

static int mfg_pref_set_u32(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    uint32_t value) {
    mfg_pref_context_t *context = (mfg_pref_context_t *)ns->user;
    assert(strcmp(key, "mfg_acceptance_revision") == 0);
    context->pending_revision = value;
    context->pending_revision_set = 1;
    return H2_PAL_OK;
}

static int mfg_pref_commit(h2_pal_pref_namespace_t *ns) {
    mfg_pref_context_t *context = (mfg_pref_context_t *)ns->user;
    context->commits += 1;
    if (context->commit_result != H2_PAL_OK) {
        return context->commit_result;
    }
    memcpy(context->persisted, context->pending, context->pending_len);
    context->persisted_len = context->pending_len;
    if (context->pending_revision_set) {
        context->persisted_revision = context->pending_revision;
        context->revision_present = 1;
        context->pending_revision_set = 0;
    }
    return H2_PAL_OK;
}

static int mfg_pref_open(
    void *user,
    const char *name_space,
    h2_pal_pref_open_mode_t mode,
    h2_pal_pref_namespace_t **out_namespace) {
    mfg_pref_context_t *context = (mfg_pref_context_t *)user;
    (void)mode;
    assert(strcmp(name_space, "h2loader") == 0);
    context->pref_namespace = (h2_pal_pref_namespace_t){
        .user = context,
        .close = mfg_pref_close,
        .get_blob = mfg_pref_get_blob,
        .set_blob = mfg_pref_set_blob,
        .get_u32 = mfg_pref_get_u32,
        .set_u32 = mfg_pref_set_u32,
        .commit = mfg_pref_commit,
    };
    *out_namespace = &context->pref_namespace;
    return H2_PAL_OK;
}

static int stage_test_open(
    void *user,
    const char *path,
    h2_pal_fs_open_mode_t mode,
    h2_pal_fs_file_t **out_file) {
    stage_test_context_t *context = user;
    assert(strcmp(path, "/dl/update.tar.zlib.tmp") == 0);
    assert(mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE);
    context->opens++;
    context->package_present_when_opened = context->package_exists;
    context->temporary_exists = 1;
    context->temporary_size = 0u;
    *out_file = &context->file;
    return H2_PAL_FS_OK;
}

static int stage_test_write(
    void *user,
    h2_pal_fs_file_t *file,
    const void *data,
    size_t len,
    size_t *out_written) {
    stage_test_context_t *context = user;
    uint64_t used = context->package_size + context->previous_size +
        context->temporary_size;
    (void)file;
    (void)data;
    if (used > context->capacity ||
        len > context->capacity - used) {
        *out_written = 0u;
        return H2_PAL_ERR_NO_SPACE;
    }
    context->temporary_size += len;
    context->writes++;
    *out_written = len;
    return H2_PAL_FS_OK;
}

static int stage_test_sync(void *user, h2_pal_fs_file_t *file) {
    (void)user;
    (void)file;
    return H2_PAL_FS_OK;
}

static int stage_test_close(void *user, h2_pal_fs_file_t *file) {
    stage_test_context_t *context = user;
    (void)file;
    context->closes++;
    return H2_PAL_FS_OK;
}

static int stage_test_remove(void *user, const char *path) {
    stage_test_context_t *context = user;

    context->removes++;
    if (strcmp(path, "/dl/update.tar.zlib") == 0) {
        if (!context->package_exists) {
            return H2_PAL_FS_ERR_NOT_FOUND;
        }
        context->package_exists = 0;
        context->package_size = 0u;
        return H2_PAL_FS_OK;
    }
    if (strcmp(path, "/dl/update.tar.zlib.tmp") == 0) {
        if (!context->temporary_exists) {
            return H2_PAL_FS_ERR_NOT_FOUND;
        }
        context->temporary_exists = 0;
        context->temporary_size = 0u;
        return H2_PAL_FS_OK;
    }
    if (strcmp(path, "/dl/update.tar.zlib.prev") == 0) {
        if (!context->previous_exists) {
            return H2_PAL_FS_ERR_NOT_FOUND;
        }
        context->previous_exists = 0;
        context->previous_size = 0u;
        return H2_PAL_FS_OK;
    }
    return H2_PAL_FS_ERR_NOT_FOUND;
}

static int stage_test_stat(
    void *user,
    const char *path,
    h2_pal_fs_stat_t *out_stat) {
    stage_test_context_t *context = user;

    memset(out_stat, 0, sizeof(*out_stat));
    if (strcmp(path, "/dl/update.tar.zlib") == 0 &&
        context->package_exists) {
        out_stat->size = context->package_size;
        out_stat->is_dir = context->package_is_dir;
        return H2_PAL_FS_OK;
    }
    if (strcmp(path, "/dl/update.tar.zlib.tmp") == 0 &&
        context->temporary_exists) {
        out_stat->size = context->temporary_size;
        out_stat->is_dir = context->temporary_is_dir;
        return H2_PAL_FS_OK;
    }
    if (strcmp(path, "/dl/update.tar.zlib.prev") == 0 &&
        context->previous_exists) {
        out_stat->size = context->previous_size;
        out_stat->is_dir = context->previous_is_dir;
        return H2_PAL_FS_OK;
    }
    return H2_PAL_FS_ERR_NOT_FOUND;
}

static int stage_test_rename(
    void *user,
    const char *old_path,
    const char *new_path) {
    stage_test_context_t *context = user;

    if (strcmp(old_path, "/dl/update.tar.zlib.tmp") == 0 &&
        strcmp(new_path, "/dl/update.tar.zlib") == 0 &&
        context->temporary_exists) {
        context->package_exists = 1;
        context->package_size = context->temporary_size;
        context->temporary_exists = 0;
        context->temporary_size = 0u;
        return H2_PAL_FS_OK;
    }
    return H2_PAL_FS_ERR_NOT_FOUND;
}

static int stage_test_pref_close(h2_pal_pref_namespace_t *ns) {
    (void)ns;
    return H2_PAL_FS_OK;
}

static int stage_test_pref_get_u32(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    uint32_t *out_value) {
    stage_test_context_t *context = ns->user;

    if (strcmp(key, "boot_intent") == 0) {
        if (context->boot_intent == H2_LOADER_BOOT_INTENT_UNKNOWN) {
            return H2_PAL_ERR_NOT_FOUND;
        }
        *out_value = (uint32_t)context->boot_intent;
        return H2_PAL_OK;
    }
    if (strcmp(key, "install_state") == 0) {
        *out_value = (uint32_t)context->install_state;
        return H2_PAL_OK;
    }
    if (strcmp(key, "installed_size") == 0 && context->installed_valid) {
        *out_value = context->installed_size;
        return H2_PAL_OK;
    }
    if (strcmp(key, "staged_size") == 0 && context->staged_valid) {
        *out_value = context->staged_size;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static int stage_test_pref_set_u32(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    uint32_t value) {
    stage_test_context_t *context = ns->user;

    if (context->pref_set_error != H2_PAL_OK) {
        return context->pref_set_error;
    }
    if (strcmp(key, "boot_intent") == 0) {
        context->boot_intent = (h2_loader_boot_intent_t)value;
        return H2_PAL_OK;
    }
    if (strcmp(key, "install_state") == 0) {
        context->install_state = (h2_loader_install_state_t)value;
        return H2_PAL_OK;
    }
    if (strcmp(key, "staged_size") == 0) {
        context->staged_size = value;
        context->staged_valid = 1;
        return H2_PAL_OK;
    }
    return H2_PAL_OK;
}

static int stage_test_pref_get_i32(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    int32_t *out_value) {
    stage_test_context_t *context = ns->user;

    if (strcmp(key, "last_result") != 0) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_value = context->last_result;
    return H2_PAL_OK;
}

static int stage_test_pref_get_bool(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    int *out_value) {
    stage_test_context_t *context = ns->user;

    if (strcmp(key, "app_confirmed") == 0) {
        *out_value = context->app_confirmed;
        return H2_PAL_OK;
    }
    if (strcmp(key, "manual_hold") == 0) {
        *out_value = context->manual_hold;
        return H2_PAL_OK;
    }
    if (strcmp(key, "publish_committed") == 0 &&
        context->publish_marker_present) {
        *out_value = context->publish_committed;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static int stage_test_pref_set_bool(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    int value) {
    stage_test_context_t *context = ns->user;

    if (context->pref_set_error != H2_PAL_OK) {
        return context->pref_set_error;
    }
    if (strcmp(key, "app_confirmed") == 0) {
        context->app_confirmed = value;
        return H2_PAL_OK;
    }
    if (strcmp(key, "manual_hold") == 0) {
        context->manual_hold = value;
        return H2_PAL_OK;
    }
    if (strcmp(key, "publish_committed") == 0) {
        context->publish_marker_present = 1;
        context->publish_committed = value;
        return H2_PAL_OK;
    }
    return H2_PAL_OK;
}

static int stage_test_pref_get_string(
    h2_pal_pref_namespace_t *ns,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    char **out_value) {
    stage_test_context_t *context = ns->user;
    const char *value = NULL;

    *out_value = NULL;
    if (strcmp(key, "installed_version") == 0 &&
        context->installed_valid) {
        value = context->installed_version;
    } else if (strcmp(key, "installed_checksum") == 0 &&
        context->installed_valid) {
        value = context->installed_checksum;
    } else if (strcmp(key, "staged_version") == 0 &&
        context->staged_valid) {
        value = context->staged_version;
    } else if (strcmp(key, "staged_checksum") == 0 &&
        context->staged_valid) {
        value = context->staged_checksum;
    }
    if (value == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_value = h2_pal_mem_alloc(allocator, strlen(value) + 1u);
    if (*out_value == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    strcpy(*out_value, value);
    return H2_PAL_OK;
}

static int stage_test_pref_set_string(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    const char *value) {
    stage_test_context_t *context = ns->user;

    if (context->pref_set_error != H2_PAL_OK) {
        return context->pref_set_error;
    }
    if (strcmp(key, "staged_version") == 0) {
        strcpy(context->staged_version, value);
        context->staged_valid = 1;
        return H2_PAL_OK;
    }
    if (strcmp(key, "staged_checksum") == 0) {
        strcpy(context->staged_checksum, value);
        context->staged_valid = 1;
        return H2_PAL_OK;
    }
    return H2_PAL_OK;
}

static int stage_test_pref_get_blob(
    h2_pal_pref_namespace_t *ns,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    void **out_data,
    size_t *out_len) {
    stage_test_context_t *context = ns->user;

    *out_data = NULL;
    *out_len = 0u;
    if (strcmp(key, "loader_upgrade") == 0 &&
        context->upgrade_record_len != 0u) {
        *out_data = h2_pal_mem_alloc(allocator, context->upgrade_record_len);
        if (*out_data == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(*out_data, context->upgrade_record,
               context->upgrade_record_len);
        *out_len = context->upgrade_record_len;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static int stage_test_pref_set_blob(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    const void *data,
    size_t len) {
    stage_test_context_t *context = ns->user;

    if (strcmp(key, "loader_upgrade") != 0 || data == NULL ||
        len > sizeof(context->upgrade_record)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(context->upgrade_record, data, len);
    context->upgrade_record_len = len;
    return H2_PAL_OK;
}

static int stage_test_pref_remove(
    h2_pal_pref_namespace_t *ns,
    const char *key) {
    stage_test_context_t *context = ns->user;

    if (strcmp(key, "staged_version") == 0 ||
        strcmp(key, "staged_checksum") == 0 ||
        strcmp(key, "staged_size") == 0) {
        if (!context->staged_valid) {
            return H2_PAL_ERR_NOT_FOUND;
        }
        if (strcmp(key, "staged_version") == 0) {
            context->staged_version[0] = '\0';
        } else if (strcmp(key, "staged_checksum") == 0) {
            context->staged_checksum[0] = '\0';
        } else {
            context->staged_size = 0u;
        }
        if (context->staged_version[0] == '\0' &&
            context->staged_checksum[0] == '\0' &&
            context->staged_size == 0u) {
            context->staged_valid = 0;
        }
        return H2_PAL_OK;
    }
    if (strcmp(key, "publish_committed") == 0) {
        if (!context->publish_marker_present) {
            return H2_PAL_ERR_NOT_FOUND;
        }
        context->publish_marker_present = 0;
        context->publish_committed = 0;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static int stage_test_pref_commit(h2_pal_pref_namespace_t *ns) {
    stage_test_context_t *context = ns->user;

    context->pref_commits++;
    return context->pref_commit_error;
}

static int stage_test_pref_open(
    void *user,
    const char *name_space,
    h2_pal_pref_open_mode_t mode,
    h2_pal_pref_namespace_t **out_namespace) {
    stage_test_context_t *context = user;
    (void)mode;

    assert(strcmp(name_space, H2_LOADER_PREF_NAMESPACE) == 0);
    context->pref_namespace = (h2_pal_pref_namespace_t){
        .user = context,
        .close = stage_test_pref_close,
        .get_u32 = stage_test_pref_get_u32,
        .set_u32 = stage_test_pref_set_u32,
        .get_i32 = stage_test_pref_get_i32,
        .get_bool = stage_test_pref_get_bool,
        .set_bool = stage_test_pref_set_bool,
        .get_string = stage_test_pref_get_string,
        .set_string = stage_test_pref_set_string,
        .get_blob = stage_test_pref_get_blob,
        .set_blob = stage_test_pref_set_blob,
        .remove = stage_test_pref_remove,
        .commit = stage_test_pref_commit,
    };
    *out_namespace = &context->pref_namespace;
    return H2_PAL_OK;
}

static h2_pal_result_t stage_test_power_running(
    void *user,
    h2_pal_power_boot_partition_t *out_partition) {
    stage_test_context_t *context = user;

    memset(out_partition, 0, sizeof(*out_partition));
    out_partition->id = context->running_partition;
    return H2_PAL_OK;
}

static h2_pal_result_t stage_test_power_next(
    void *user,
    h2_pal_power_boot_partition_t *out_partition) {
    stage_test_context_t *context = user;

    memset(out_partition, 0, sizeof(*out_partition));
    out_partition->id = context->next_partition;
    return H2_PAL_OK;
}

static int stage_test_wifi_status(
    void *user,
    h2_pal_wifi_sta_status_t *out_status) {
    stage_test_context_t *context = user;

    context->wifi_status_calls++;
    context->package_present_when_wifi_checked = context->package_exists;
    memset(out_status, 0, sizeof(*out_status));
    return H2_PAL_ERR_UNAVAILABLE;
}

static void *stage_test_mem_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void stage_test_mem_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static void stage_test_fixture_init(stage_test_fixture_t *fixture) {
    static const h2_pal_fs_vtable_t fs_vtable = {
        .open = stage_test_open,
        .write = stage_test_write,
        .sync = stage_test_sync,
        .close = stage_test_close,
        .stat = stage_test_stat,
        .remove = stage_test_remove,
        .rename = stage_test_rename,
    };
    static const h2_pal_pref_vtable_t pref_vtable = {
        .open = stage_test_pref_open,
    };
    static const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = stage_test_power_running,
        .get_next_boot_partition = stage_test_power_next,
    };
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = stage_test_mem_alloc,
        .free = stage_test_mem_free,
    };
    static const h2_pal_http_vtable_t http_vtable = {0};
    static const h2_pal_wifi_sta_vtable_t wifi_vtable = {
        .get_status = stage_test_wifi_status,
    };
    static const h2_pal_disk_vtable_t disk_vtable = {0};

    memset(fixture, 0, sizeof(*fixture));
    fixture->context.capacity = 16u;
    fixture->context.boot_intent = H2_LOADER_BOOT_INTENT_APP;
    fixture->context.install_state = H2_LOADER_INSTALL_STATE_CONFIRMED;
    fixture->context.app_confirmed = 1;
    fixture->context.installed_valid = 1;
    strcpy(fixture->context.installed_version, "installed");
    strcpy(fixture->context.installed_checksum, TEST_CD_SHA256);
    fixture->context.installed_size = 8u;
    fixture->context.running_partition = 1u;
    fixture->context.next_partition = 1u;
    fixture->fs = (h2_pal_fs_api_t){
        .user = &fixture->context,
        .vtable = &fs_vtable,
    };
    fixture->pref = (h2_pal_pref_api_t){
        .user = &fixture->context,
        .vtable = &pref_vtable,
    };
    fixture->power = (h2_pal_power_api_t){
        .user = &fixture->context,
        .vtable = &power_vtable,
    };
    fixture->mem = (h2_pal_mem_api_t){
        .vtable = &mem_vtable,
    };
    fixture->http = (h2_pal_http_api_t){
        .vtable = &http_vtable,
    };
    fixture->wifi = (h2_pal_wifi_sta_api_t){
        .user = &fixture->context,
        .vtable = &wifi_vtable,
    };
    fixture->disk = (h2_pal_disk_api_t){
        .vtable = &disk_vtable,
    };
    fixture->loader.config.package.fs = &fixture->fs;
    fixture->loader.config.package.allocator = &fixture->mem;
    fixture->loader.config.package.package_path =
        H2_LOADER_DEFAULT_PACKAGE_PATH;
    fixture->loader.config.pref = &fixture->pref;
    fixture->loader.config.power = &fixture->power;
    fixture->loader.config.board = "test";
    fixture->loader.config.target = "host";
    fixture->loader.config.chip = "native";
    fixture->loader.config.hardware_capabilities = H2_LOADER_CAPABILITY_UART;
    fixture->loader.config.active_identity.role =
        H2_LOADER_IMAGE_ROLE_H2LOADER;
    strcpy(fixture->loader.config.active_identity.version, "test");
    strcpy(fixture->loader.config.active_identity.image_sha256,
        TEST_AB_SHA256);
    fixture->loader.package.config =
        fixture->loader.config.package;
}

static int idle_pref_close(h2_pal_pref_namespace_t *ns) {
    (void)ns;
    return H2_PAL_OK;
}

static int idle_pref_get_blob(
    h2_pal_pref_namespace_t *ns,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    void **out_data,
    size_t *out_len) {
    (void)ns;
    *out_data = NULL;
    *out_len = 0u;
    idle_trial_context_t *context = (idle_trial_context_t *)ns->user;
    if (strcmp(key, "loader_upgrade") == 0 &&
        context->upgrade_record_len != 0u) {
        void *data = h2_pal_mem_alloc(allocator, context->upgrade_record_len);
        if (data == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(data, context->upgrade_record, context->upgrade_record_len);
        *out_data = data;
        *out_len = context->upgrade_record_len;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static int idle_pref_set_blob(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    const void *data,
    size_t len) {
    idle_trial_context_t *context = (idle_trial_context_t *)ns->user;
    if (strcmp(key, "loader_upgrade") != 0 || data == NULL ||
        len > sizeof(context->upgrade_record)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(context->upgrade_record, data, len);
    context->upgrade_record_len = len;
    return H2_PAL_OK;
}

static int idle_pref_remove(
    h2_pal_pref_namespace_t *ns,
    const char *key) {
    idle_trial_context_t *context = (idle_trial_context_t *)ns->user;
    if (strcmp(key, "loader_upgrade") == 0) {
        if (context->upgrade_record_len == 0u) {
            return H2_PAL_ERR_NOT_FOUND;
        }
        context->upgrade_record_len = 0u;
        return H2_PAL_OK;
    }
    if (strcmp(key, "loader_upgrade_step") != 0) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (!context->upgrade_step_present) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    context->upgrade_step[0] = '\0';
    context->upgrade_step_present = 0;
    context->upgrade_step_removes += 1;
    return H2_PAL_OK;
}

static void *idle_mem_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void idle_mem_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static int idle_pref_set_u32(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    uint32_t value) {
    idle_trial_context_t *context = (idle_trial_context_t *)ns->user;
    context->lifecycle_set_calls += 1;
    if (context->lifecycle_set_fail_after != 0 &&
        context->lifecycle_set_calls == context->lifecycle_set_fail_after) {
        return context->lifecycle_set_result;
    }
    if (strcmp(key, "install_state") == 0) {
        context->pending_install_state = value;
        context->pending_lifecycle = 1;
    } else if (strcmp(key, "boot_intent") == 0) {
        context->pending_boot_intent = (h2_loader_boot_intent_t)value;
        context->pending_lifecycle = 1;
    }
    return H2_PAL_OK;
}

static int idle_pref_get_u32(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    uint32_t *out_value) {
    idle_trial_context_t *context = (idle_trial_context_t *)ns->user;
    if (strcmp(key, "install_state") == 0) {
        *out_value = context->install_state;
        return H2_PAL_OK;
    }
    if (strcmp(key, "boot_intent") == 0) {
        *out_value = (uint32_t)context->boot_intent;
        return H2_PAL_OK;
    }
    if (strcmp(key, "installed_size") == 0 && context->installed_valid) {
        *out_value = context->installed_size;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static int idle_pref_get_string(
    h2_pal_pref_namespace_t *ns,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    char **out_value) {
    idle_trial_context_t *context = (idle_trial_context_t *)ns->user;
    const char *value = NULL;

    *out_value = NULL;
    if (strcmp(key, "installed_version") == 0 &&
        context->installed_valid) {
        value = context->installed_version;
    } else if (strcmp(key, "installed_checksum") == 0 &&
               context->installed_valid) {
        value = context->installed_checksum;
    }
    if (value == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_value = h2_pal_mem_alloc(allocator, strlen(value) + 1u);
    if (*out_value == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    strcpy(*out_value, value);
    return H2_PAL_OK;
}

static int idle_pref_get_bool(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    int *out_value) {
    idle_trial_context_t *context = (idle_trial_context_t *)ns->user;
    if (strcmp(key, "app_confirmed") == 0) {
        *out_value = context->app_confirmed;
        return H2_PAL_OK;
    }
    if (strcmp(key, "manual_hold") == 0) {
        *out_value = context->manual_hold;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static int idle_pref_set_bool(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    int value) {
    idle_trial_context_t *context = (idle_trial_context_t *)ns->user;
    context->lifecycle_set_calls += 1;
    if (context->lifecycle_set_fail_after != 0 &&
        context->lifecycle_set_calls == context->lifecycle_set_fail_after) {
        return context->lifecycle_set_result;
    }
    if (strcmp(key, "app_confirmed") == 0) {
        context->pending_app_confirmed = value;
        context->pending_lifecycle = 1;
    } else if (strcmp(key, "manual_hold") == 0) {
        context->pending_manual_hold = value;
        context->pending_lifecycle = 1;
    }
    return H2_PAL_OK;
}

static int idle_pref_set_i32(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    int32_t value) {
    idle_trial_context_t *context = (idle_trial_context_t *)ns->user;
    assert(strcmp(key, "last_result") == 0);
    context->last_result = value;
    context->last_result_sets += 1;
    return H2_PAL_OK;
}

static int idle_pref_commit(h2_pal_pref_namespace_t *ns) {
    idle_trial_context_t *context = (idle_trial_context_t *)ns->user;
    context->commits += 1;
    if (context->commit_result != H2_PAL_OK) {
        return context->commit_result;
    }
    if (context->pending_lifecycle) {
        context->boot_intent = context->pending_boot_intent;
        context->install_state = context->pending_install_state;
        context->app_confirmed = context->pending_app_confirmed;
        context->manual_hold = context->pending_manual_hold;
    }
    context->commit_sequence = ++context->sequence;
    return H2_PAL_OK;
}

static int idle_pref_open(
    void *user,
    const char *name_space,
    h2_pal_pref_open_mode_t mode,
    h2_pal_pref_namespace_t **out_namespace) {
    idle_trial_context_t *context = (idle_trial_context_t *)user;
    (void)name_space;
    (void)mode;
    context->pending_boot_intent = context->boot_intent;
    context->pending_install_state = context->install_state;
    context->pending_app_confirmed = context->app_confirmed;
    context->pending_manual_hold = context->manual_hold;
    context->pending_lifecycle = 0;
    context->pref_namespace.user = context;
    context->pref_namespace.close = idle_pref_close;
    context->pref_namespace.get_blob = idle_pref_get_blob;
    context->pref_namespace.set_blob = idle_pref_set_blob;
    context->pref_namespace.get_u32 = idle_pref_get_u32;
    context->pref_namespace.get_bool = idle_pref_get_bool;
    context->pref_namespace.get_string = idle_pref_get_string;
    context->pref_namespace.set_u32 = idle_pref_set_u32;
    context->pref_namespace.set_i32 = idle_pref_set_i32;
    context->pref_namespace.set_bool = idle_pref_set_bool;
    context->pref_namespace.remove = idle_pref_remove;
    context->pref_namespace.commit = idle_pref_commit;
    *out_namespace = &context->pref_namespace;
    return H2_PAL_OK;
}

static h2_pal_result_t idle_power_running(
    void *user,
    h2_pal_power_boot_partition_t *out_partition) {
    idle_trial_context_t *context = (idle_trial_context_t *)user;
    memset(out_partition, 0, sizeof(*out_partition));
    out_partition->id = context->running_partition;
    return H2_PAL_OK;
}

static h2_pal_result_t idle_power_select(void *user, uint32_t partition_id) {
    idle_trial_context_t *context = (idle_trial_context_t *)user;
    context->selected_partition = partition_id;
    context->boot_selections += 1;
    context->selection_sequence = ++context->sequence;
    return H2_PAL_OK;
}

static h2_pal_result_t idle_power_set_hold(void *user, int enabled) {
    idle_trial_context_t *context = (idle_trial_context_t *)user;
    context->hold_calls += 1;
    context->hold_value = enabled;
    return context->hold_result;
}

static h2_pal_result_t idle_power_reboot(void *user, uint32_t reason) {
    idle_trial_context_t *context = (idle_trial_context_t *)user;
    (void)reason;
    context->reboots += 1;
    context->reboot_sequence = ++context->sequence;
    return context->reboot_result;
}

static int idle_reboot_transition(void *user) {
    idle_trial_context_t *context = (idle_trial_context_t *)user;
    context->reboot_transitions += 1;
    context->reboots_at_transition = context->reboots;
    context->transition_sequence = ++context->sequence;
    return context->reboot_transition_result;
}

static int idle_before_disruptive(
    void *user,
    h2_loader_disruptive_action_t action) {
    idle_trial_context_t *context = (idle_trial_context_t *)user;
    context->disruptive_calls += 1;
    context->disruptive_action = action;
    context->disruptive_sequence = ++context->sequence;
    return context->disruptive_result;
}

static int idle_confirm_active(void *user) {
    idle_trial_context_t *context = (idle_trial_context_t *)user;
    context->confirms += 1;
    return context->confirm_result;
}

static int idle_fs_remove(void *user, const char *path) {
    (void)user;
    assert(strcmp(path, H2_LOADER_DEFAULT_PACKAGE_PATH) == 0);
    return H2_PAL_FS_ERR_NOT_FOUND;
}

static int test_fs_open(
    void *user,
    const char *path,
    h2_pal_fs_open_mode_t mode,
    h2_pal_fs_file_t **out_file) {
    checksum_test_context_t *context = (checksum_test_context_t *)user;
    assert(strcmp(path, H2_LOADER_DEFAULT_PACKAGE_PATH) == 0);
    assert(mode == H2_PAL_FS_OPEN_READ);
    context->file.offset = 0u;
    *out_file = &context->file;
    return H2_PAL_FS_OK;
}

static int test_fs_read(
    void *user,
    h2_pal_fs_file_t *file,
    void *data,
    size_t len,
    size_t *out_read) {
    checksum_test_context_t *context = (checksum_test_context_t *)user;
    size_t remaining = context->len - file->offset;
    size_t copy_len = remaining < len ? remaining : len;
    memcpy(data, context->data + file->offset, copy_len);
    file->offset += copy_len;
    *out_read = copy_len;
    return H2_PAL_FS_OK;
}

static int test_fs_close(void *user, h2_pal_fs_file_t *file) {
    checksum_test_context_t *context = (checksum_test_context_t *)user;
    (void)file;
    context->closed += 1;
    return H2_PAL_FS_OK;
}

static int test_digest_start(void *user) {
    (void)user;
    return H2_PAL_OK;
}

static int test_digest_update(void *user, const uint8_t *data, size_t len) {
    (void)user;
    (void)data;
    (void)len;
    return H2_PAL_OK;
}

static int test_digest_finish(void *user, uint8_t out_digest[32]) {
    (void)user;
    memset(out_digest, 0, 32u);
    return H2_PAL_OK;
}

static void test_digest_abort(void *user) {
    checksum_test_context_t *context = (checksum_test_context_t *)user;
    context->aborted += 1;
}

static int validation_digest_start(void *user) {
    validation_digest_context_t *context = (validation_digest_context_t *)user;
    context->len = 0u;
    return H2_PAL_OK;
}

static int validation_digest_update(void *user, const uint8_t *data, size_t len) {
    validation_digest_context_t *context = (validation_digest_context_t *)user;
    if (data == NULL || len > sizeof(context->data) - context->len) {
        return H2_PAL_ERR_NO_SPACE;
    }
    memcpy(context->data + context->len, data, len);
    context->len += len;
    return H2_PAL_OK;
}

static int validation_digest_finish(void *user, uint8_t out_digest[32]) {
    static const uint8_t expected_data[] = "data/version.txt\0v2\0";
    static const uint8_t expected_app[] = "loader";
    validation_digest_context_t *context = (validation_digest_context_t *)user;
    uint8_t fill = 0xefu;

    if (context->len == 0u ||
        (context->len == sizeof(expected_data) - 1u &&
            memcmp(context->data, expected_data, sizeof(expected_data) - 1u) == 0)) {
        fill = 0xabu;
    } else if (context->len == sizeof(expected_app) - 1u &&
        memcmp(context->data, expected_app, sizeof(expected_app) - 1u) == 0) {
        fill = 0xcdu;
    }
    memset(out_digest, fill, 32u);
    return H2_PAL_OK;
}

static void validation_digest_abort(void *user) {
    validation_digest_context_t *context = (validation_digest_context_t *)user;
    context->aborts += 1;
}

static size_t append_test_tar_file(
    uint8_t *tar,
    size_t capacity,
    size_t offset,
    const char *path,
    const void *data,
    size_t len) {
    uint8_t *header;
    uint64_t sum = 0u;
    size_t padded_len = (len + 511u) & ~(size_t)511u;

    assert(tar != NULL && path != NULL && strlen(path) < 100u);
    assert(offset <= capacity && 512u + padded_len <= capacity - offset);
    header = tar + offset;
    memset(header, 0, 512u + padded_len);
    memcpy(header, path, strlen(path));
    (void)snprintf((char *)header + 124u, 12u, "%011llo",
        (unsigned long long)len);
    memset(header + 148u, ' ', 8u);
    header[156u] = '0';
    for (size_t i = 0u; i < 512u; ++i) {
        sum += header[i];
    }
    (void)snprintf((char *)header + 148u, 8u, "%06llo",
        (unsigned long long)sum);
    header[154u] = '\0';
    header[155u] = ' ';
    if (len > 0u) {
        assert(data != NULL);
        memcpy(header + 512u, data, len);
    }
    return offset + 512u + padded_len;
}

static size_t wrap_test_zlib(
    uint8_t *out,
    size_t capacity,
    const uint8_t *data,
    size_t len) {
    uint32_t sum1 = 1u;
    uint32_t sum2 = 0u;
    uint16_t block_len;
    uint16_t inverse_len;

    assert(out != NULL && data != NULL && len <= UINT16_MAX);
    assert(capacity >= len + 11u);
    block_len = (uint16_t)len;
    inverse_len = (uint16_t)~block_len;
    out[0] = 0x78u;
    out[1] = 0x01u;
    out[2] = 0x01u;
    out[3] = (uint8_t)block_len;
    out[4] = (uint8_t)(block_len >> 8u);
    out[5] = (uint8_t)inverse_len;
    out[6] = (uint8_t)(inverse_len >> 8u);
    memcpy(out + 7u, data, len);
    for (size_t i = 0u; i < len; ++i) {
        sum1 = (sum1 + data[i]) % 65521u;
        sum2 = (sum2 + sum1) % 65521u;
    }
    out[len + 7u] = (uint8_t)(sum2 >> 8u);
    out[len + 8u] = (uint8_t)sum2;
    out[len + 9u] = (uint8_t)(sum1 >> 8u);
    out[len + 10u] = (uint8_t)sum1;
    return len + 11u;
}

static int validate_test_package_with_allocator(
    const char *data,
    size_t data_len,
    const char *checksum,
    const h2_pal_mem_api_t *allocator) {
    static const char manifest[] =
        "format=1\n"
        "role=app\n"
        "board=szp\n"
        "target=esp32s3\n"
        "version=v2\n"
        "image_size=6\n"
        "image_sha256=" TEST_CD_SHA256 "\n";
    static const char app[] = "loader";
    uint8_t tar[8192];
    uint8_t compressed[sizeof(tar) + 11u];
    size_t tar_len = 0u;
    size_t compressed_len;
    validation_digest_context_t digest_context = {0};
    checksum_test_context_t fs_context = {0};
    h2_loader_package_t package = {0};
    const h2_pal_fs_vtable_t fs_vtable = {
        .open = test_fs_open,
        .read = test_fs_read,
        .close = test_fs_close,
    };
    const h2_pal_fs_api_t fs = {.user = &fs_context, .vtable = &fs_vtable};

    memset(tar, 0, sizeof(tar));
    tar_len = append_test_tar_file(tar, sizeof(tar), tar_len,
        "manifest", manifest, sizeof(manifest) - 1u);
    tar_len = append_test_tar_file(tar, sizeof(tar), tar_len,
        "checksum", checksum, strlen(checksum));
    if (data != NULL) {
        tar_len = append_test_tar_file(tar, sizeof(tar), tar_len,
            "data/version.txt", data, data_len);
    }
    tar_len = append_test_tar_file(tar, sizeof(tar), tar_len,
        "app/esp/app.bin", app, sizeof(app) - 1u);
    tar_len += 1024u;
    compressed_len = wrap_test_zlib(
        compressed, sizeof(compressed), tar, tar_len);

    fs_context.data = compressed;
    fs_context.len = compressed_len;
    package.config.fs = &fs;
    package.config.allocator = allocator;
    package.config.app_entry_path = H2_LOADER_DEFAULT_APP_ENTRY_PATH;
    package.config.digest = (h2_loader_digest_api_t){
        .user = &digest_context,
        .start = validation_digest_start,
        .update = validation_digest_update,
        .finish = validation_digest_finish,
        .abort = validation_digest_abort,
    };
    return h2_loader_package_validate_path(
        &package, H2_LOADER_DEFAULT_PACKAGE_PATH);
}

static int validate_test_package(
    const char *data,
    size_t data_len,
    const char *checksum) {
    return validate_test_package_with_allocator(
        data, data_len, checksum, NULL);
}

static void *reject_mem_alloc(void *user, size_t len) {
    (void)user;
    (void)len;
    return NULL;
}

static void test_package_validation_uses_configured_allocator(void) {
    static const char checksum[] = TEST_AB_SHA256 "\n";
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = reject_mem_alloc,
        .free = test_mem_free,
    };
    static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};

    assert(validate_test_package_with_allocator(
               "v2", 2u, checksum, &mem) == H2_BUNDLE_ERR_ZLIB);
}

static void test_package_validation_recomputes_data_digest(void) {
    static const char checksum[] = TEST_AB_SHA256 "\n";

    assert(validate_test_package("v2", 2u, checksum) == H2_PAL_OK);
    assert(validate_test_package("v3", 2u, checksum) == H2_BUNDLE_ERR_LAYOUT);
    assert(validate_test_package(NULL, 0u, checksum) == H2_PAL_OK);
}

static void disk_digest_abort(void *user) {
    (void)user;
}

static h2_pal_result_t progress_disk_get_partition(
    void *user,
    uint32_t partition_id,
    h2_pal_disk_partition_t *out_partition) {
    (void)user;
    if ((partition_id != 1u && partition_id != 2u) || out_partition == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_partition, 0, sizeof(*out_partition));
    out_partition->id = partition_id;
    out_partition->size = sizeof(((disk_progress_context_t *)0)->source);
    out_partition->erase_block_size = 256u;
    return H2_PAL_OK;
}

static h2_pal_result_t progress_disk_read(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    void *data,
    size_t len) {
    disk_progress_context_t *context = (disk_progress_context_t *)user;
    const uint8_t *partition = partition_id == 1u
        ? context->source : context->destination;
    if ((partition_id != 1u && partition_id != 2u) || data == NULL ||
        offset > sizeof(context->source) ||
        len > sizeof(context->source) - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len > context->max_read_len) {
        context->max_read_len = len;
    }
    memcpy(data, partition + offset, len);
    return H2_PAL_OK;
}

static h2_pal_result_t progress_disk_erase(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    uint64_t len) {
    disk_progress_context_t *context = (disk_progress_context_t *)user;
    if (partition_id != 2u || offset > sizeof(context->destination) ||
        len > sizeof(context->destination) - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(context->destination + offset, 0xff, (size_t)len);
    return H2_PAL_OK;
}

static h2_pal_result_t progress_disk_write(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    const void *data,
    size_t len) {
    disk_progress_context_t *context = (disk_progress_context_t *)user;
    if (partition_id != 2u || data == NULL ||
        offset > sizeof(context->destination) ||
        len > sizeof(context->destination) - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len > context->max_write_len) {
        context->max_write_len = len;
    }
    memcpy(context->destination + offset, data, len);
    return H2_PAL_OK;
}

static h2_pal_result_t progress_disk_flush(void *user,
                                           uint32_t partition_id) {
    (void)user;
    return partition_id == 2u ? H2_PAL_OK : H2_PAL_ERR_INVALID_ARG;
}

static void record_install_progress(
    void *user,
    h2_loader_install_phase_t phase,
    uint64_t completed,
    uint64_t total,
    const char *detail) {
    disk_progress_context_t *context = (disk_progress_context_t *)user;
    if (phase != H2_LOADER_INSTALL_PHASE_IMAGE) {
        return;
    }
    assert(detail != NULL);
    assert(total == sizeof(context->source));
    assert(completed >= context->last_completed);
    context->last_completed = completed;
    context->last_total = total;
    context->image_progress_calls += 1;
}

static void test_disk_writer_reports_image_progress(void) {
    disk_progress_context_t context = {0};
    h2_loader_package_t package = {0};
    h2_loader_image_identity_t identity = {0};
    const h2_pal_disk_vtable_t disk_vtable = {
        .get_partition = progress_disk_get_partition,
        .read = progress_disk_read,
        .erase = progress_disk_erase,
        .write = progress_disk_write,
        .flush = progress_disk_flush,
    };
    const h2_pal_disk_api_t disk = {
        .user = &context,
        .vtable = &disk_vtable,
    };
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_mem_alloc,
        .free = test_mem_free,
    };
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};

    for (size_t i = 0u; i < sizeof(context.source); ++i) {
        context.source[i] = (uint8_t)i;
    }
    memset(context.destination, 0xa5, sizeof(context.destination));
    package.config.disk = &disk;
    package.config.allocator = &mem;
    package.config.app_entry_path = H2_LOADER_DEFAULT_APP_ENTRY_PATH;
    package.config.digest = (h2_loader_digest_api_t){
        .start = test_digest_start,
        .update = test_digest_update,
        .finish = test_digest_finish,
        .abort = disk_digest_abort,
    };
    package.config.progress_user = &context;
    package.config.progress = record_install_progress;
    identity.image_size = sizeof(context.source);
    memset(identity.image_sha256, '0', H2_LOADER_SHA256_HEX_SIZE - 1u);

    assert(h2_loader_image_copy_to(&package, &identity, 1u, 2u) == H2_PAL_OK);
    assert(memcmp(context.source, context.destination,
        sizeof(context.source)) == 0);
    assert(context.image_progress_calls == 2);
    assert(context.last_completed == sizeof(context.source));
    assert(context.last_total == sizeof(context.source));
    assert(context.max_read_len == sizeof(context.source));
    assert(context.max_write_len == sizeof(context.source));
}

static void test_install_rejects_changed_staged_archive(void) {
    static const uint8_t archive[] = {1u, 2u, 3u, 4u};
    checksum_test_context_t context = {
        .data = archive,
        .len = sizeof(archive),
    };
    const h2_pal_fs_vtable_t fs_vtable = {
        .open = test_fs_open,
        .read = test_fs_read,
        .close = test_fs_close,
    };
    const h2_pal_fs_api_t fs = {
        .user = &context,
        .vtable = &fs_vtable,
    };
    h2_loader_package_t package = {
        .config = {
            .fs = &fs,
            .package_path = H2_LOADER_DEFAULT_PACKAGE_PATH,
            .digest = {
                .user = &context,
                .start = test_digest_start,
                .update = test_digest_update,
                .finish = test_digest_finish,
                .abort = test_digest_abort,
            },
        },
    };
    h2_loader_identity_t identity = {
        .valid = 1,
        .checksum = "1111111111111111111111111111111111111111111111111111111111111111",
        .size = sizeof(archive),
    };

    assert(h2_loader_package_install_staged(&package, &identity) == H2_PAL_ERR_FORMAT);
    assert(context.closed == 1);
    assert(context.aborted == 1);
}

static void test_manifest_parser(void) {
    static const char valid[] =
        "format=1\n"
        "role=h2loader\n"
        "board=amoled\n"
        "target=esp32s3\n"
        "version=2026.07.16\n"
        "image_size=4096\n"
        "image_sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
    static const char reordered[] =
        "role=h2loader\n"
        "format=1\n"
        "board=amoled\n"
        "target=esp32s3\n"
        "version=2026.07.16\n"
        "image_size=4096\n"
        "image_sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
    static const char crlf[] =
        "format=1\r\n"
        "role=h2loader\r\n"
        "board=amoled\r\n"
        "target=esp32s3\r\n"
        "version=2026.07.16\r\n"
        "image_size=4096\r\n"
        "image_sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\r\n";
    static const char version_with_space[] =
        "format=1\n"
        "role=h2loader\n"
        "board=amoled\n"
        "target=esp32s3\n"
        "version=2026.07.16 release\n"
        "image_size=4096\n"
        "image_sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
    h2_loader_package_manifest_t manifest;

    assert(h2_loader_package_manifest_parse(valid, strlen(valid), &manifest) == H2_PAL_OK);
    assert(manifest.role == H2_LOADER_IMAGE_ROLE_H2LOADER);
    assert(strcmp(manifest.board, "amoled") == 0);
    assert(strcmp(manifest.target, "esp32s3") == 0);
    assert(manifest.image_size == 4096u);
    assert(h2_loader_package_manifest_parse(crlf, strlen(crlf), &manifest) == H2_PAL_OK);
    assert(strcmp(manifest.version, "2026.07.16") == 0);
    assert(h2_loader_package_manifest_parse(
        version_with_space,
        strlen(version_with_space),
        &manifest) == H2_PAL_ERR_FORMAT);
    assert(h2_loader_package_manifest_parse(reordered, strlen(reordered), &manifest) == H2_PAL_ERR_FORMAT);
    assert(h2_loader_package_manifest_parse(valid, strlen(valid) - 1u, &manifest) == H2_PAL_ERR_FORMAT);
}

static void test_upgrade_record_codec(void) {
    uint8_t encoded[384];
    h2_loader_upgrade_record_t record;
    h2_loader_upgrade_record_t decoded;
    size_t encoded_len = 0u;

    memset(&record, 0, sizeof(record));
    record.format = 1u;
    record.phase = H2_LOADER_UPGRADE_PHASE_TRIAL_PENDING;
    strcpy(record.package_sha256, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    record.candidate.format = 1u;
    record.candidate.role = H2_LOADER_IMAGE_ROLE_H2LOADER;
    strcpy(record.candidate.board, "amoled");
    strcpy(record.candidate.target, "esp32s3");
    strcpy(record.candidate.version, "2026.07.16");
    record.candidate.image_size = 4096u;
    strcpy(record.candidate.image_sha256, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    record.canonical_partition = 1u;
    record.trial_partition = 2u;
    record.last_result = -7;

    assert(h2_loader_upgrade_record_encode(
        &record,
        encoded,
        sizeof(encoded),
        &encoded_len) == H2_PAL_OK);
    assert(h2_loader_upgrade_record_decode(encoded, encoded_len, &decoded) == H2_PAL_OK);
    assert(decoded.phase == record.phase);
    assert(decoded.last_result == record.last_result);
    assert(strcmp(decoded.candidate.image_sha256, record.candidate.image_sha256) == 0);
    assert(h2_loader_upgrade_record_decode(encoded, encoded_len - 1u, &decoded) == H2_PAL_ERR_FORMAT);
    encoded[encoded_len] = 0u;
    assert(h2_loader_upgrade_record_decode(encoded, encoded_len + 1u, &decoded) == H2_PAL_ERR_FORMAT);
    memset(record.candidate.version, 'a', sizeof(record.candidate.version));
    assert(h2_loader_upgrade_record_encode(
        &record,
        encoded,
        sizeof(encoded),
        &encoded_len) == H2_PAL_ERR_FORMAT);
}

static h2_loader_mfg_summary_t mfg_summary_with_prefix(uint32_t passed) {
    h2_loader_mfg_summary_t summary = {.total = H2_LOADER_MFG_STEP_TOTAL};
    for (uint32_t i = 0u; i < passed; ++i) {
        summary.step_status[i] = H2_LOADER_MFG_STEP_PASSED;
    }
    return summary;
}

static void test_mfg_record_round_trip_and_commit_failure(void) {
    mfg_pref_context_t context = {0};
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_mem_alloc,
        .free = test_mem_free,
    };
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_pal_pref_vtable_t pref_vtable = {.open = mfg_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    h2_loader_mfg_summary_t expected = mfg_summary_with_prefix(3u);
    expected.step_status[3] = H2_LOADER_MFG_STEP_SKIPPED;
    h2_loader_mfg_summary_t actual = {0};
    int present = 0;

    assert(h2_loader_mfg_write(&pref, &expected) == H2_PAL_OK);
    assert(context.commits == 1);
    assert(context.persisted_len == 4u + H2_LOADER_MFG_STEP_TOTAL);
    assert(context.persisted[0] == 3u && context.persisted[1] == 0u &&
           context.persisted[2] == 0u && context.persisted[3] == 0u);
    assert(memcmp(context.persisted + 4u, expected.step_status,
                  sizeof(expected.step_status)) == 0);
    assert(h2_loader_mfg_read(&pref, &mem, &actual, &present) == H2_PAL_OK);
    assert(present);
    assert(actual.total == expected.total);
    assert(memcmp(actual.step_status, expected.step_status,
                  sizeof(actual.step_status)) == 0);

    context.commit_result = H2_PAL_ERR_WRITE;
    const h2_loader_mfg_summary_t replacement =
        mfg_summary_with_prefix(H2_LOADER_MFG_STEP_TOTAL);
    assert(h2_loader_mfg_write(&pref, &replacement) == H2_PAL_ERR_WRITE);
    memset(&actual, 0, sizeof(actual));
    assert(h2_loader_mfg_read(&pref, &mem, &actual, &present) == H2_PAL_OK);
    assert(memcmp(actual.step_status, expected.step_status,
                  sizeof(actual.step_status)) == 0);

    context.commit_result = H2_PAL_OK;
    for (uint32_t passed = 0u; passed <= H2_LOADER_MFG_STEP_TOTAL; ++passed) {
        const h2_loader_mfg_summary_t partial =
            mfg_summary_with_prefix(passed);
        assert(h2_loader_mfg_write(&pref, &partial) == H2_PAL_OK);
        assert(h2_loader_mfg_read(&pref, &mem, &actual, &present) == H2_PAL_OK);
        assert(memcmp(actual.step_status, partial.step_status,
                      sizeof(actual.step_status)) == 0);
    }
    assert(h2_loader_mfg_reset(&pref, H2_LOADER_MFG_STEP_TOTAL) == H2_PAL_OK);
    assert(h2_loader_mfg_read(&pref, &mem, &actual, &present) == H2_PAL_OK);
    assert(actual.total == H2_LOADER_MFG_STEP_TOTAL);
    for (uint32_t i = 0u; i < H2_LOADER_MFG_STEP_TOTAL; ++i) {
        assert(actual.step_status[i] == H2_LOADER_MFG_STEP_UNTESTED);
    }
}

static void test_mfg_v3_record_rejects_wrong_length_or_status(void) {
    mfg_pref_context_t context = {0};
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_mem_alloc,
        .free = test_mem_free,
    };
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_pal_pref_vtable_t pref_vtable = {.open = mfg_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_loader_mfg_summary_t summary = mfg_summary_with_prefix(3u);
    h2_loader_mfg_summary_t actual = {0};
    int present = 0;

    assert(h2_loader_mfg_write(&pref, &summary) == H2_PAL_OK);
    context.persisted_len -= 1u;
    assert(h2_loader_mfg_read(&pref, &mem, &actual, &present) == H2_PAL_OK);
    assert(present);
    assert(actual.total == H2_LOADER_MFG_STEP_TOTAL);
    assert(context.persisted_len == 4u + H2_LOADER_MFG_STEP_TOTAL);
    for (uint32_t i = 0u; i < H2_LOADER_MFG_STEP_TOTAL; ++i) {
        assert(actual.step_status[i] == H2_LOADER_MFG_STEP_UNTESTED);
    }

    assert(h2_loader_mfg_write(&pref, &summary) == H2_PAL_OK);
    context.persisted[4] = 4u;
    assert(h2_loader_mfg_read(&pref, &mem, &actual, &present) == H2_PAL_OK);
    assert(actual.total == H2_LOADER_MFG_STEP_TOTAL);
    for (uint32_t i = 0u; i < H2_LOADER_MFG_STEP_TOTAL; ++i) {
        assert(actual.step_status[i] == H2_LOADER_MFG_STEP_UNTESTED);
    }

    assert(h2_loader_mfg_write(&pref, &summary) == H2_PAL_OK);
    context.persisted[0] = 9u;
    assert(h2_loader_mfg_read(&pref, &mem, &actual, &present) == H2_PAL_OK);
    assert(actual.total == H2_LOADER_MFG_STEP_TOTAL);
    assert(context.persisted[0] == 3u && context.persisted[1] == 0u &&
           context.persisted[2] == 0u && context.persisted[3] == 0u);
    for (uint32_t i = 0u; i < H2_LOADER_MFG_STEP_TOTAL; ++i) {
        assert(actual.step_status[i] == H2_LOADER_MFG_STEP_UNTESTED);
    }
}

static void test_mfg_legacy_record_reconstructs_passed_prefix(void) {
    mfg_pref_context_t context = {0};
    const uint8_t legacy_record[16] = {
        1u, 0u, 0u, 0u,
        1u, 0u, 0u, 0u,
        3u, 0u, 0u, 0u,
        H2_LOADER_MFG_STEP_TOTAL, 0u, 0u, 0u,
    };
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_mem_alloc,
        .free = test_mem_free,
    };
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_pal_pref_vtable_t pref_vtable = {.open = mfg_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    h2_loader_mfg_summary_t actual = {0};
    int present = 0;

    memcpy(context.persisted, legacy_record, sizeof(legacy_record));
    context.persisted_len = sizeof(legacy_record);
    assert(h2_loader_mfg_read(&pref, &mem, &actual, &present) == H2_PAL_OK);
    assert(present);
    assert(actual.total == H2_LOADER_MFG_STEP_TOTAL);
    assert(actual.step_status[0] == H2_LOADER_MFG_STEP_PASSED);
    assert(actual.step_status[1] == H2_LOADER_MFG_STEP_PASSED);
    assert(actual.step_status[2] == H2_LOADER_MFG_STEP_PASSED);
    assert(actual.step_status[3] == H2_LOADER_MFG_STEP_UNTESTED);
    assert(context.persisted_len == 4u + H2_LOADER_MFG_STEP_TOTAL);
}

static void test_mfg_v2_record_migrates_masks_to_slots(void) {
    mfg_pref_context_t context = {0};
    const uint8_t legacy_record[24] = {
        2u, 0u, 0u, 0u,
        1u, 0u, 0u, 0u,
        2u, 0u, 0u, 0u,
        H2_LOADER_MFG_STEP_TOTAL, 0u, 0u, 0u,
        5u, 0u, 0u, 0u,
        2u, 0u, 0u, 0u,
    };
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_mem_alloc,
        .free = test_mem_free,
    };
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_pal_pref_vtable_t pref_vtable = {.open = mfg_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    h2_loader_mfg_summary_t actual = {0};
    int present = 0;

    memcpy(context.persisted, legacy_record, sizeof(legacy_record));
    context.persisted_len = sizeof(legacy_record);
    assert(h2_loader_mfg_read(&pref, &mem, &actual, &present) == H2_PAL_OK);
    assert(present);
    assert(actual.step_status[0] == H2_LOADER_MFG_STEP_PASSED);
    assert(actual.step_status[1] == H2_LOADER_MFG_STEP_SKIPPED);
    assert(actual.step_status[2] == H2_LOADER_MFG_STEP_PASSED);
    assert(context.persisted_len == 4u + H2_LOADER_MFG_STEP_TOTAL);
}

static void test_mfg_acceptance_revision_invalidates_obsolete_pass(void) {
    mfg_pref_context_t context = {
        .persisted_revision = 1u,
        .revision_present = 1,
    };
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_mem_alloc,
        .free = test_mem_free,
    };
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_pal_pref_vtable_t pref_vtable = {.open = mfg_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_loader_mfg_summary_t obsolete_pass =
        mfg_summary_with_prefix(H2_LOADER_MFG_STEP_TOTAL);
    h2_loader_mfg_summary_t actual = {0};
    int present = 0;

    assert(h2_loader_mfg_write(&pref, &obsolete_pass) == H2_PAL_OK);
    assert(h2_loader_mfg_ensure_acceptance_revision(
               &pref, H2_LOADER_MFG_STEP_TOTAL, 2u) ==
           H2_PAL_OK);
    assert(context.persisted_revision == 2u);
    assert(context.commits == 3);
    assert(h2_loader_mfg_read(&pref, &mem, &actual, &present) == H2_PAL_OK);
    assert(present);
    assert(actual.total == H2_LOADER_MFG_STEP_TOTAL);
    for (uint32_t i = 0u; i < H2_LOADER_MFG_STEP_TOTAL; ++i) {
        assert(actual.step_status[i] == H2_LOADER_MFG_STEP_UNTESTED);
    }

    assert(h2_loader_mfg_ensure_acceptance_revision(
               &pref, H2_LOADER_MFG_STEP_TOTAL, 2u) ==
           H2_PAL_OK);
    assert(context.commits == 3);
    assert(h2_loader_mfg_ensure_acceptance_revision(
               &pref, H2_LOADER_MFG_STEP_TOTAL, 0u) ==
           H2_PAL_ERR_INVALID_ARG);
}

static void test_mfg_summary_validation_and_status_format(void) {
    h2_loader_status_t status = {0};
    char output[H2_LOADER_STATUS_LINE_MAX];
    strcpy(status.board, "format-board");
    strcpy(status.target, "format-target");
    strcpy(status.chip, "format-chip");
    status.capabilities = H2_LOADER_CAPABILITY_UART;
    status.command_availability = H2_LOADER_COMMAND_AVAILABLE_STATUS;
    status.active_role = H2_LOADER_ACTIVE_ROLE_APP;
    status.app_confirmed = 1;
    status.mfg = mfg_summary_with_prefix(H2_LOADER_MFG_STEP_TOTAL);
    assert(h2_loader_mfg_summary_is_passed(
        &status.mfg, H2_LOADER_MFG_STEP_TOTAL));
    assert(!h2_loader_mfg_summary_is_passed(&status.mfg, 10u));
    assert(h2_loader_status_format(&status, output, sizeof(output)) == H2_PAL_OK);
    assert(strncmp(
               output,
               "H2_LOADER_STATUS board=format-board target=format-target "
               "chip=format-chip capabilities=0x00000001 "
               "command_availability=0x00000008 ",
               strlen(
                   "H2_LOADER_STATUS board=format-board target=format-target "
                   "chip=format-chip capabilities=0x00000001 "
                   "command_availability=0x00000008 ")) == 0);
    assert(strstr(output, "states=0x") != NULL);
    assert(h2_loader_states_pack(&status, &status.states) == H2_PAL_OK);
    assert(h2_loader_states_app_confirmed(status.states));
    assert(h2_loader_states_mfg_step(status.states, 0u) ==
           H2_LOADER_MFG_STEP_PASSED);
    status.mfg.step_status[7] = H2_LOADER_MFG_STEP_SKIPPED;
    assert(h2_loader_status_format(&status, output, sizeof(output)) == H2_PAL_OK);
    assert(!h2_loader_mfg_summary_is_passed(
        &status.mfg, H2_LOADER_MFG_STEP_TOTAL));
    status.mfg.step_status[7] = 4u;
    assert(h2_loader_mfg_summary_validate(&status.mfg) ==
           H2_PAL_ERR_INVALID_ARG);
}

static void test_mfg_handoff_pending_truth_table(void) {
    h2_loader_status_t status = {0};
    int pending = 7;

    assert(h2_loader_status_mfg_handoff_pending(NULL, 0, &pending) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_loader_status_mfg_handoff_pending(&status, 0, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_loader_status_mfg_handoff_pending(&status, -1, &pending) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_loader_status_mfg_handoff_pending(&status, 2, &pending) ==
           H2_PAL_ERR_INVALID_ARG);

    status.boot_intent = H2_LOADER_BOOT_INTENT_APP;
    status.install_state = H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED;
    assert(h2_loader_status_mfg_handoff_pending(&status, 1, &pending) ==
           H2_PAL_OK);
    assert(pending == 0);

    status.boot_intent = H2_LOADER_BOOT_INTENT_H2LOADER;
    assert(h2_loader_status_mfg_handoff_pending(&status, 0, &pending) ==
           H2_PAL_OK);
    assert(pending == 0);

    status.boot_intent = H2_LOADER_BOOT_INTENT_APP;
    for (int state = H2_LOADER_INSTALL_STATE_IDLE;
         state <= H2_LOADER_INSTALL_STATE_MAIN_FAILED;
         ++state) {
        status.install_state = (h2_loader_install_state_t)state;
        status.app_confirmed = 0;
        status.installed.valid = 0;
        assert(h2_loader_status_mfg_handoff_pending(&status, 0, &pending) ==
               H2_PAL_OK);
        assert(pending ==
               (state == H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED ||
                state == H2_LOADER_INSTALL_STATE_INSTALLING));
    }

    status.install_state = H2_LOADER_INSTALL_STATE_CONFIRMED;
    status.app_confirmed = 1;
    assert(h2_loader_status_mfg_handoff_pending(&status, 0, &pending) ==
           H2_PAL_OK);
    assert(pending == 0);
    status.installed.valid = 1;
    assert(h2_loader_status_mfg_handoff_pending(&status, 0, &pending) ==
           H2_PAL_OK);
    assert(pending == 1);
    status.app_confirmed = 0;
    assert(h2_loader_status_mfg_handoff_pending(&status, 0, &pending) ==
           H2_PAL_OK);
    assert(pending == 0);
}

static void test_status_format_fits_shared_line_capacity(void) {
    h2_loader_status_t status = {0};
    char output[H2_LOADER_STATUS_LINE_MAX];

    memset(status.board, 'b', sizeof(status.board) - 1u);
    memset(status.target, 't', sizeof(status.target) - 1u);
    memset(status.chip, 'c', sizeof(status.chip) - 1u);
    status.active_role = H2_LOADER_ACTIVE_ROLE_H2LOADER;
    memset(status.active_name, 'n', sizeof(status.active_name) - 1u);
    memset(status.active_version, 'v', sizeof(status.active_version) - 1u);
    memset(status.active_checksum, 'a', sizeof(status.active_checksum) - 1u);
    memset(status.installed.version, 'i', sizeof(status.installed.version) - 1u);
    memset(status.installed.checksum, 'j', sizeof(status.installed.checksum) - 1u);
    memset(status.staged.version, 's', sizeof(status.staged.version) - 1u);
    memset(status.staged.checksum, 'k', sizeof(status.staged.checksum) - 1u);
    memset(status.loader_upgrade.candidate.board, 'd',
        sizeof(status.loader_upgrade.candidate.board) - 1u);
    memset(status.loader_upgrade.candidate.target, 'e',
        sizeof(status.loader_upgrade.candidate.target) - 1u);
    memset(status.loader_upgrade.candidate.version, 'f',
        sizeof(status.loader_upgrade.candidate.version) - 1u);
    memset(status.loader_upgrade.package_sha256, 'p',
        sizeof(status.loader_upgrade.package_sha256) - 1u);
    memset(status.loader_upgrade_step, 'u',
        sizeof(status.loader_upgrade_step) - 1u);
    memset(status.loader_upgrade.candidate.image_sha256, '0',
        sizeof(status.loader_upgrade.candidate.image_sha256) - 1u);
    status.mfg = (h2_loader_mfg_summary_t){
        .total = H2_LOADER_MFG_STEP_TOTAL,
    };
    status.command_availability =
        H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP |
        H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER;

    assert(h2_loader_status_format(&status, output, sizeof(output)) == H2_PAL_OK);
    assert(strstr(
        output, "command_availability=0x00000003") != NULL);
    assert(strstr(output, "states=0x") != NULL);
}

static void test_command_availability_flags(void) {
    idle_trial_context_t context = {
        .running_partition = 2u,
        .boot_intent = H2_LOADER_BOOT_INTENT_APP,
        .commit_result = H2_PAL_ERR_WRITE,
        .installed_valid = 1,
        .installed_version = "installed",
        .installed_checksum = TEST_CD_SHA256,
        .installed_size = 8u,
    };
    h2_loader_t loader = {0};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = idle_power_running,
        .set_next_boot_partition = idle_power_select,
        .reboot = idle_power_reboot,
    };
    const h2_pal_power_api_t power = {.user = &context, .vtable = &power_vtable};
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = idle_mem_alloc,
        .free = idle_mem_free,
    };
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};

    loader.config.power = &power;
    loader.config.pref = &pref;
    loader.config.package.allocator = &mem;
    loader.config.h2loader_partition_id = 1u;
    loader.config.hardware_capabilities = H2_LOADER_CAPABILITY_UART;
    loader.status.capabilities = H2_LOADER_CAPABILITY_UART;
    assert(h2_loader_set_implemented_commands(
               &loader, H2_LOADER_COMMAND_AVAILABILITY_ALL) == H2_PAL_OK);

    assert(h2_loader_set_command_availability(
               NULL,
               H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP,
               false) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_loader_set_command_availability(
               &loader, 0u, false) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_loader_set_command_availability(
               &loader, UINT32_C(1) << 20, false) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_loader_set_command_availability(
               &loader,
               H2_LOADER_COMMAND_AVAILABILITY_ALL,
               false) == H2_PAL_OK);
    loader.config.active_identity.role = H2_LOADER_IMAGE_ROLE_H2LOADER;
    loader.config.app_partition_id = 2u;
    loader.status.staged.valid = 1;
    assert(h2_loader_reboot_h2loader_with_transition(
               &loader, idle_reboot_transition, &context) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(h2_loader_request_install_staged(&loader) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(context.boot_selections == 0);
    assert(context.commits == 0);

    assert(h2_loader_set_command_availability(
               &loader,
               H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER,
               true) == H2_PAL_OK);
    assert(h2_loader_reboot_h2loader_with_transition(
               &loader, idle_reboot_transition, &context) ==
           H2_PAL_ERR_WRITE);
    assert(context.boot_selections == 1);
    assert(context.commits == 1);
    assert(h2_loader_request_install_staged(&loader) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(context.commits == 1);

    assert(h2_loader_set_command_availability(
               &loader,
               H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP,
               true) == H2_PAL_OK);
    assert(h2_loader_request_install_staged(&loader) == H2_PAL_ERR_WRITE);
    assert(context.commits == 2);
}

static void test_command_availability_reflects_loader_state(void) {
    stage_test_fixture_t fixture;
    h2_loader_status_t status;

    stage_test_fixture_init(&fixture);
    fixture.loader.config.hardware_capabilities = H2_LOADER_CAPABILITY_UART;
    assert(h2_loader_set_implemented_commands(
               &fixture.loader, H2_LOADER_COMMAND_AVAILABILITY_ALL) ==
           H2_PAL_OK);
    fixture.context.installed_valid = 0;

    assert(h2_loader_read_status(&fixture.loader, &status) == H2_PAL_OK);
    assert((status.command_availability &
            H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER) != 0u);
    assert((status.command_availability &
            H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) == 0u);
    assert((status.command_availability &
            H2_LOADER_COMMAND_AVAILABLE_STAGE_ABORT) == 0u);
    assert(h2_loader_request_install_staged(&fixture.loader) ==
           H2_PAL_ERR_INVALID_STATE);

    fixture.context.installed_valid = 1;
    assert(h2_loader_read_status(&fixture.loader, &status) == H2_PAL_OK);
    assert((status.command_availability &
            H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) != 0u);

    fixture.context.staged_valid = 1;
    strcpy(fixture.context.staged_version, "staged");
    strcpy(fixture.context.staged_checksum, TEST_AB_SHA256);
    fixture.context.staged_size = 8u;
    fixture.context.package_exists = 1;
    fixture.context.package_size = 8u;
    assert(h2_loader_read_status(&fixture.loader, &status) == H2_PAL_OK);
    assert((status.command_availability &
            H2_LOADER_COMMAND_AVAILABLE_STAGE_ABORT) != 0u);
    assert((status.command_availability &
            H2_LOADER_COMMAND_AVAILABLE_LOADER_UPGRADE) != 0u);

    assert(h2_loader_set_command_availability(
               &fixture.loader,
               H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP,
               false) == H2_PAL_OK);
    assert(h2_loader_read_status(&fixture.loader, &status) == H2_PAL_OK);
    assert((status.command_availability &
            H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) == 0u);
    assert((status.command_availability &
            H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER) != 0u);
    assert(h2_loader_set_command_availability(
               &fixture.loader,
               H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP,
               true) == H2_PAL_OK);

    fixture.loader.config.mfg_required_total = 1u;
    assert(h2_loader_read_status(&fixture.loader, &status) == H2_PAL_OK);
    assert((status.command_availability &
            H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) == 0u);
    assert((status.command_availability &
            H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER) != 0u);
    assert(h2_loader_set_implemented_commands(&fixture.loader, 0u) == H2_PAL_OK);
    assert(h2_loader_read_status(&fixture.loader, &status) == H2_PAL_OK);
    assert(status.command_availability == 0u);
}

static void test_hardware_capabilities_are_stable(void) {
    stage_test_fixture_t fixture;
    h2_loader_status_t status;

    stage_test_fixture_init(&fixture);
    fixture.loader.config.hardware_capabilities =
        H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI;
    assert(h2_loader_set_implemented_commands(
               &fixture.loader, H2_LOADER_COMMAND_AVAILABILITY_ALL) ==
           H2_PAL_OK);
    assert(h2_loader_read_status(&fixture.loader, &status) == H2_PAL_OK);
    assert(status.capabilities ==
           (H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI));
    assert(h2_loader_set_command_availability(
               &fixture.loader,
               H2_LOADER_COMMAND_AVAILABLE_STATUS,
               false) == H2_PAL_OK);
    assert(h2_loader_read_status(&fixture.loader, &status) == H2_PAL_OK);
    assert(status.capabilities ==
           (H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI));
    assert((status.command_availability &
            H2_LOADER_COMMAND_AVAILABLE_STATUS) == 0u);
}

static void assert_command_availability_gate(
    h2_loader_command_t *command,
    h2_loader_t *loader,
    uint32_t command_flag,
    size_t argc,
    const char *const *argv) {
    assert(h2_loader_set_command_availability(
               loader, command_flag, false) == H2_PAL_OK);
    assert(h2_loader_command_execute(command, argc, argv) != H2_PAL_OK);
    assert(h2_loader_set_command_availability(
               loader, command_flag, true) == H2_PAL_OK);
}

static void test_loader_commands_follow_command_availability(void) {
    static const char *const status[] = {"h2loader", "status"};
    static const char *const stage[] = {"h2loader", "stage", "abort"};
    static const char *const upgrade[] = {"h2loader", "upgrade"};
    static const char *const reboot[] = {"h2loader", "reboot", "loader"};
    static const char *const hold[] = {"h2loader", "hold", "on"};
    static const char *const coredump[] = {"h2loader", "coredump", "status"};
    h2_loader_t loader = {0};
    h2_loader_command_t command;
    command_test_io_t io = {0};

    loader.config.hardware_capabilities = H2_LOADER_CAPABILITY_UART;
    assert(command_test_init(&command, &loader, &io) == H2_PAL_OK);
    assert_command_availability_gate(
        &command, &loader, H2_LOADER_COMMAND_AVAILABLE_STATUS, 2u, status);
    assert_command_availability_gate(
        &command, &loader, H2_LOADER_COMMAND_AVAILABLE_STAGE_ABORT, 3u, stage);
    assert_command_availability_gate(
        &command, &loader, H2_LOADER_COMMAND_AVAILABLE_LOADER_UPGRADE, 2u, upgrade);
    assert_command_availability_gate(
        &command, &loader, H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER, 3u, reboot);
    assert_command_availability_gate(
        &command, &loader, H2_LOADER_COMMAND_AVAILABLE_HOLD_ON, 3u, hold);
    assert_command_availability_gate(
        &command, &loader, H2_LOADER_COMMAND_AVAILABLE_COREDUMP_STATUS, 3u, coredump);
}

static void test_loader_commands_recheck_availability_after_operation_lock(void) {
    static const char *const argv[] = {"h2loader", "hold", "on"};
    idle_trial_context_t idle_context = {0};
    h2_loader_t loader = {0};
    h2_loader_command_t command;
    command_test_io_t io = {0};
    capability_lock_test_context_t lock_context = {
        .loader = &loader,
        .command = H2_LOADER_COMMAND_AVAILABLE_HOLD_ON,
    };
    const h2_pal_sync_vtable_t sync_vtable = {
        .lock_mutex = capability_lock_test_lock,
        .unlock_mutex = capability_lock_test_unlock,
    };
    const h2_pal_sync_api_t sync = {
        .user = &lock_context,
        .vtable = &sync_vtable,
    };
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {
        .user = &idle_context,
        .vtable = &pref_vtable,
    };
    const h2_pal_power_vtable_t power_vtable = {
        .set_hold = idle_power_set_hold,
    };
    const h2_pal_power_api_t power = {
        .user = &idle_context,
        .vtable = &power_vtable,
    };

    loader.config.pref = &pref;
    loader.config.power = &power;
    loader.config.hardware_capabilities = H2_LOADER_CAPABILITY_UART;
    assert(command_test_init(&command, &loader, &io) == H2_PAL_OK);
    command.config.operation_sync = &sync;
    command.config.operation_mutex = (h2_pal_mutex_t *)&lock_context;

    assert(h2_loader_command_execute(&command, 3u, argv) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(lock_context.lock_calls == 1u);
    assert(lock_context.unlock_calls == 1u);
    assert(idle_context.hold_calls == 0);
}

static void test_reboot_h2loader_commits_and_acknowledges_before_teardown(void) {
    idle_trial_context_t context = {
        .running_partition = 2u,
        .boot_intent = H2_LOADER_BOOT_INTENT_APP,
    };
    h2_loader_t loader = {0};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = idle_power_running,
        .set_next_boot_partition = idle_power_select,
        .reboot = idle_power_reboot,
    };
    const h2_pal_power_api_t power = {.user = &context, .vtable = &power_vtable};
    loader.config.power = &power;
    loader.config.pref = &pref;
    loader.config.h2loader_partition_id = 1u;
    loader.config.hardware_capabilities = H2_LOADER_CAPABILITY_UART;
    loader.config.disruptive_user = &context;
    loader.config.before_disruptive = idle_before_disruptive;

    context.disruptive_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_loader_boot_h2loader(&loader) == H2_PAL_ERR_TIMEOUT);
    assert(context.disruptive_calls == 1);
    assert(context.boot_selections == 0);
    assert(context.reboots == 0);

    context.commit_result = H2_PAL_ERR_WRITE;
    assert(h2_loader_reboot_h2loader_with_transition(
        &loader, idle_reboot_transition, &context) == H2_PAL_ERR_WRITE);
    assert(context.boot_selections == 1);
    assert(context.boot_intent == H2_LOADER_BOOT_INTENT_APP);
    assert(context.reboot_transitions == 0);
    assert(context.disruptive_calls == 1);
    assert(context.reboots == 0);

    context.commit_result = H2_PAL_OK;
    context.disruptive_result = H2_PAL_OK;
    context.reboot_transition_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_loader_reboot_h2loader_with_transition(
        &loader, idle_reboot_transition, &context) == H2_PAL_ERR_TIMEOUT);
    assert(context.boot_selections == 2);
    assert(context.boot_intent == H2_LOADER_BOOT_INTENT_H2LOADER);
    assert(context.reboot_transitions == 1);
    assert(context.selection_sequence < context.commit_sequence);
    assert(context.commit_sequence < context.transition_sequence);
    assert(context.disruptive_calls == 1);
    assert(context.reboots_at_transition == 0);
    assert(context.reboots == 0);

    context.reboot_transition_result = H2_PAL_OK;
    context.disruptive_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_loader_reboot_h2loader_with_transition(
        &loader, idle_reboot_transition, &context) == H2_PAL_ERR_TIMEOUT);
    assert(context.boot_selections == 3);
    assert(context.reboot_transitions == 2);
    assert(context.transition_sequence < context.disruptive_sequence);
    assert(context.disruptive_calls == 2);
    assert(context.reboots == 0);

    context.running_partition = 1u;
    context.disruptive_result = H2_PAL_OK;
    context.reboot_result = H2_PAL_ERR_IO;
    assert(h2_loader_reboot_h2loader_with_transition(
        &loader, idle_reboot_transition, &context) == H2_PAL_ERR_IO);
    assert(context.disruptive_calls == 3);
    assert(context.disruptive_action == H2_LOADER_DISRUPTIVE_BOOT_H2LOADER);
    assert(context.boot_selections == 3);
    assert(context.reboot_transitions == 3);
    assert(context.reboots_at_transition == 0);
    assert(context.reboots == 1);
    assert(context.commit_sequence < context.transition_sequence);
    assert(context.transition_sequence < context.disruptive_sequence);
    assert(context.disruptive_sequence < context.reboot_sequence);
}

static void test_loader_reboot_command_bypasses_incomplete_mfg_gate(void) {
    static const char *const argv[] = {"h2loader", "reboot", "loader"};
    idle_trial_context_t context = {.running_partition = 1u};
    h2_loader_t loader = {0};
    h2_loader_command_t command;
    command_test_io_t io = {0};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = idle_power_running,
        .set_next_boot_partition = idle_power_select,
        .reboot = idle_power_reboot,
    };
    const h2_pal_power_api_t power = {.user = &context, .vtable = &power_vtable};

    loader.config.pref = &pref;
    loader.config.power = &power;
    loader.config.h2loader_partition_id = 1u;
    loader.config.hardware_capabilities = H2_LOADER_CAPABILITY_UART;
    loader.config.mfg_required_total = 8u;
    loader.config.disruptive_user = &context;
    loader.config.before_disruptive = idle_before_disruptive;

    assert(command_test_init(&command, &loader, &io) == H2_PAL_OK);
    assert(h2_loader_set_command_availability(
               &loader,
               H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER,
               false) == H2_PAL_OK);
    assert(h2_loader_command_execute(&command, 3u, argv) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(strstr(io.output, "result=accepted") == NULL);
    assert(io.output_len == 0u);
    assert(context.disruptive_calls == 0);
    assert(context.reboots == 0);
    assert(h2_loader_set_command_availability(
               &loader,
               H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER,
               true) == H2_PAL_OK);
    memset(io.output, 0, sizeof(io.output));
    io.output_len = 0u;
    assert(h2_loader_command_execute(&command, 3u, argv) == H2_PAL_OK);
    assert(context.disruptive_calls == 1);
    assert(context.disruptive_action == H2_LOADER_DISRUPTIVE_BOOT_H2LOADER);
    assert(context.boot_selections == 0);
    assert(context.reboots == 1);
    assert(strstr(io.output, "H2_LOADER_REBOOT target=loader result=accepted") != NULL);
    assert(strstr(io.output, "H2_LOADER_REBOOT_FINAL target=loader result=OK") != NULL);

    context.disruptive_result = H2_PAL_ERR_TIMEOUT;
    memset(io.output, 0, sizeof(io.output));
    io.output_len = 0u;
    assert(h2_loader_command_execute(&command, 3u, argv) == H2_PAL_ERR_TIMEOUT);
    assert(strstr(io.output, "result=accepted") != NULL);
    assert(strstr(io.output, "H2_LOADER_REBOOT_FINAL target=loader result=fail") != NULL);
    assert(context.reboots == 1);

    context.disruptive_result = H2_PAL_OK;
    context.reboot_result = H2_PAL_ERR_IO;
    memset(io.output, 0, sizeof(io.output));
    io.output_len = 0u;
    assert(h2_loader_command_execute(&command, 3u, argv) == H2_PAL_ERR_IO);
    assert(strstr(io.output, "result=accepted") != NULL);
    assert(strstr(io.output, "H2_LOADER_REBOOT_FINAL target=loader result=fail") != NULL);
    assert(context.reboots == 2);
}

static void test_app_request_commits_once_before_ack_and_teardown(void) {
    idle_trial_context_t context = {
        .running_partition = 1u,
        .boot_intent = H2_LOADER_BOOT_INTENT_H2LOADER,
        .install_state = H2_LOADER_INSTALL_STATE_IDLE,
        .manual_hold = 1,
        .installed_valid = 1,
        .installed_version = "installed",
        .installed_checksum = TEST_CD_SHA256,
        .installed_size = 8u,
    };
    h2_loader_t loader = {0};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = idle_power_running,
        .set_hold = idle_power_set_hold,
    };
    const h2_pal_power_api_t power = {.user = &context, .vtable = &power_vtable};
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = idle_mem_alloc,
        .free = idle_mem_free,
    };
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};

    loader.config.pref = &pref;
    loader.config.package.allocator = &mem;
    loader.config.power = &power;
    loader.config.hardware_capabilities = H2_LOADER_CAPABILITY_UART;
    loader.config.disruptive_user = &context;
    loader.config.before_disruptive = idle_before_disruptive;

    context.lifecycle_set_fail_after = 2;
    context.lifecycle_set_result = H2_PAL_ERR_WRITE;
    assert(h2_loader_request_install_staged_with_transition(
        &loader, idle_reboot_transition, &context) == H2_PAL_ERR_WRITE);
    assert(context.hold_calls == 1);
    assert(context.hold_value == 0);
    assert(context.commits == 0);
    assert(context.manual_hold == 1);
    assert(context.install_state == H2_LOADER_INSTALL_STATE_IDLE);
    assert(context.boot_intent == H2_LOADER_BOOT_INTENT_H2LOADER);
    assert(context.reboot_transitions == 0);
    assert(context.disruptive_calls == 0);

    context.lifecycle_set_fail_after = 0;
    context.lifecycle_set_calls = 0;
    context.commit_result = H2_PAL_ERR_WRITE;
    assert(h2_loader_request_install_staged_with_transition(
        &loader, idle_reboot_transition, &context) == H2_PAL_ERR_WRITE);
    assert(context.commits == 1);
    assert(context.manual_hold == 1);
    assert(context.install_state == H2_LOADER_INSTALL_STATE_IDLE);
    assert(context.boot_intent == H2_LOADER_BOOT_INTENT_H2LOADER);
    assert(context.reboot_transitions == 0);
    assert(context.disruptive_calls == 0);

    context.commit_result = H2_PAL_OK;
    context.lifecycle_set_calls = 0;
    context.reboot_transition_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_loader_request_install_staged_with_transition(
        &loader, idle_reboot_transition, &context) == H2_PAL_ERR_TIMEOUT);
    assert(context.commits == 2);
    assert(context.manual_hold == 0);
    assert(context.install_state == H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED);
    assert(context.boot_intent == H2_LOADER_BOOT_INTENT_APP);
    assert(context.reboot_transitions == 1);
    assert(context.commit_sequence < context.transition_sequence);
    assert(context.disruptive_calls == 0);

    context.reboot_transition_result = H2_PAL_OK;
    context.disruptive_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_loader_request_install_staged_with_transition(
        &loader, idle_reboot_transition, &context) == H2_PAL_ERR_TIMEOUT);
    assert(context.commits == 3);
    assert(context.reboot_transitions == 2);
    assert(context.transition_sequence < context.disruptive_sequence);
    assert(context.disruptive_calls == 1);

    context.reboot_transition_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_loader_install_staged_with_transition(
        &loader, idle_reboot_transition, &context) == H2_PAL_ERR_TIMEOUT);
    assert(context.commits == 4);
    assert(context.reboot_transitions == 3);
    assert(context.disruptive_calls == 1);
}

static void test_app_reboot_command_accepts_only_after_request_commit(void) {
    static const char *const argv[] = {"h2loader", "reboot", "app"};
    idle_trial_context_t context = {
        .installed_valid = 1,
        .installed_version = "installed",
        .installed_checksum = TEST_CD_SHA256,
        .installed_size = 8u,
    };
    h2_loader_t loader = {0};
    h2_loader_command_t command;
    command_test_io_t io = {0};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = idle_mem_alloc,
        .free = idle_mem_free,
    };
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};

    loader.config.pref = &pref;
    loader.config.package.allocator = &mem;
    loader.config.hardware_capabilities = H2_LOADER_CAPABILITY_UART;
    loader.config.disruptive_user = &context;
    loader.config.before_disruptive = idle_before_disruptive;
    assert(command_test_init(&command, &loader, &io) == H2_PAL_OK);
    command.config.defer_app_install = 1;

    context.commit_result = H2_PAL_ERR_WRITE;
    assert(h2_loader_command_execute(&command, 3u, argv) == H2_PAL_ERR_WRITE);
    assert(strstr(io.output, "result=accepted") == NULL);
    assert(strstr(
        io.output,
        "H2_LOADER_REBOOT target=app result=fail code=-14") != NULL);
    assert(io.flushes == 2u);
    assert(context.disruptive_calls == 0);

    context.commit_result = H2_PAL_OK;
    context.install_state = H2_LOADER_INSTALL_STATE_IDLE;
    memset(io.output, 0, sizeof(io.output));
    io.output_len = 0u;
    assert(h2_loader_command_execute(&command, 3u, argv) == H2_PAL_OK);
    assert(strstr(io.output,
               "H2_LOADER_REBOOT target=app result=accepted") != NULL);
    assert(io.flushes == 4u);
    assert(context.disruptive_calls == 1);
    assert(context.disruptive_action == H2_LOADER_DISRUPTIVE_BOOT_APP);
    assert(context.install_state == H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED);
}

static void test_boot_app_runs_disruptive_hook_before_reboot(void) {
    idle_trial_context_t context = {.running_partition = 1u};
    h2_loader_t loader = {0};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = idle_power_running,
        .set_next_boot_partition = idle_power_select,
        .reboot = idle_power_reboot,
    };
    const h2_pal_power_api_t power = {.user = &context, .vtable = &power_vtable};
    loader.config.power = &power;
    loader.config.pref = &pref;
    loader.config.app_partition_id = 2u;
    loader.config.disruptive_user = &context;
    loader.config.before_disruptive = idle_before_disruptive;

    context.disruptive_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_loader_boot_app(&loader) == H2_PAL_ERR_TIMEOUT);
    assert(context.disruptive_calls == 1);
    assert(context.disruptive_action == H2_LOADER_DISRUPTIVE_BOOT_APP);
    assert(context.boot_selections == 0);
    assert(context.reboots == 0);

    context.disruptive_result = H2_PAL_OK;
    assert(h2_loader_boot_app(&loader) == H2_PAL_OK);
    assert(context.disruptive_calls == 2);
    assert(context.disruptive_action == H2_LOADER_DISRUPTIVE_BOOT_APP);
    assert(context.boot_selections == 1);
    assert(context.reboots == 1);
}

static void test_upgrade_does_not_cancel_mfg_before_validation(void) {
    idle_trial_context_t context = {0};
    h2_loader_t loader = {0};
    loader.config.active_identity.role = H2_LOADER_IMAGE_ROLE_H2LOADER;
    loader.config.h2loader_partition_id = 1u;
    loader.config.app_partition_id = 2u;
    loader.config.disruptive_user = &context;
    loader.config.before_disruptive = idle_before_disruptive;
    context.disruptive_result = H2_PAL_ERR_TIMEOUT;

    assert(h2_loader_upgrade_start(&loader) != H2_PAL_OK);
    assert(context.disruptive_calls == 0);
}

static void test_last_result_is_persisted(void) {
    idle_trial_context_t context = {0};
    h2_loader_t loader = {0};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    loader.config.pref = &pref;

    assert(h2_loader_set_last_result(&loader, H2_PAL_ERR_IO) == H2_PAL_OK);
    assert(context.last_result == H2_PAL_ERR_IO);
    assert(context.last_result_sets == 1);
    assert(context.commits == 1);
    assert(loader.status.last_result == H2_PAL_ERR_IO);
    assert(h2_loader_set_last_result(NULL, H2_PAL_ERR_IO) ==
        H2_PAL_ERR_INVALID_ARG);
}

static void test_idle_trial_never_reboots_itself(void) {
    idle_trial_context_t context;
    h2_loader_t loader;
    h2_loader_startup_action_t action;
    const h2_pal_mem_vtable_t mem_vtable = {0};
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = idle_power_running,
        .set_next_boot_partition = idle_power_select,
        .reboot = idle_power_reboot,
    };
    const h2_pal_power_api_t power = {.user = &context, .vtable = &power_vtable};

    memset(&context, 0, sizeof(context));
    context.running_partition = 2u;
    memset(&loader, 0, sizeof(loader));
    loader.config.package.allocator = &mem;
    loader.config.pref = &pref;
    loader.config.power = &power;
    loader.config.h2loader_partition_id = 1u;
    loader.config.app_partition_id = 2u;
    for (size_t i = 0u; i < 3u; ++i) {
        action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
        assert(h2_loader_startup(&loader, &action) == H2_PAL_OK);
        assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
    }
    assert(context.boot_selections == 0);
    assert(context.reboots == 0);
}

static void test_confirmed_state_without_installed_identity_stays_in_command_mode(void) {
    idle_trial_context_t context = {
        .running_partition = 1u,
        .install_state = H2_LOADER_INSTALL_STATE_CONFIRMED,
        .app_confirmed = 1,
    };
    h2_loader_t loader = {0};
    h2_loader_startup_action_t action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
    const h2_pal_mem_vtable_t mem_vtable = {0};
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = idle_power_running,
        .set_next_boot_partition = idle_power_select,
        .reboot = idle_power_reboot,
    };
    const h2_pal_power_api_t power = {.user = &context, .vtable = &power_vtable};

    loader.config.package.allocator = &mem;
    loader.config.pref = &pref;
    loader.config.power = &power;
    loader.config.h2loader_partition_id = 1u;
    loader.config.app_partition_id = 2u;

    assert(h2_loader_startup(&loader, &action) == H2_PAL_OK);
    assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
    assert(context.boot_selections == 0);
    assert(context.reboots == 0);
}

static void test_failed_upgrade_does_not_block_app_startup(void) {
    idle_trial_context_t context;
    h2_loader_upgrade_record_t record;
    h2_loader_t loader;
    h2_loader_startup_action_t action;
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = idle_mem_alloc,
        .free = idle_mem_free,
    };
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = idle_power_running,
    };
    const h2_pal_power_api_t power = {.user = &context, .vtable = &power_vtable};

    memset(&context, 0, sizeof(context));
    memset(&record, 0, sizeof(record));
    record.format = 1u;
    record.phase = H2_LOADER_UPGRADE_PHASE_FAILED;
    strcpy(record.package_sha256,
           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    record.candidate.format = 1u;
    record.candidate.role = H2_LOADER_IMAGE_ROLE_H2LOADER;
    strcpy(record.candidate.board, "legacy_amoled");
    strcpy(record.candidate.target, "esp32s3");
    strcpy(record.candidate.version, "failed-upgrade");
    record.candidate.image_size = 4096u;
    strcpy(record.candidate.image_sha256,
           "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    record.canonical_partition = 1u;
    record.trial_partition = 2u;
    record.last_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_loader_upgrade_record_encode(
               &record,
               context.upgrade_record,
               sizeof(context.upgrade_record),
               &context.upgrade_record_len) == H2_PAL_OK);

    memset(&loader, 0, sizeof(loader));
    loader.config.package.allocator = &mem;
    loader.config.pref = &pref;
    loader.config.power = &power;
    loader.config.board = "amoled";
    loader.config.accepted_board_alias = "legacy_amoled";
    loader.config.target = "esp32s3";
    loader.config.h2loader_partition_id = 1u;
    loader.config.app_partition_id = 2u;
    loader.config.confirm_active_image = idle_confirm_active;
    loader.config.confirm_user = &context;
    action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
    assert(h2_loader_startup(&loader, &action) == H2_PAL_OK);
    assert(context.confirms == 1);
    assert(loader.status.loader_upgrade.phase == H2_LOADER_UPGRADE_PHASE_FAILED);
    assert(loader.status.loader_upgrade.last_result == H2_PAL_ERR_TIMEOUT);
    assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
}

static void test_successful_upgrade_recovery_clears_failure_step(void) {
    idle_trial_context_t context = {
        .running_partition = 1u,
        .upgrade_step_present = 1,
    };
    disk_progress_context_t disk_context = {0};
    h2_loader_upgrade_record_t record = {0};
    h2_loader_upgrade_record_t completed = {0};
    h2_loader_t loader = {0};
    h2_loader_startup_action_t action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_mem_alloc,
        .free = test_mem_free,
    };
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_pal_fs_vtable_t fs_vtable = {.remove = idle_fs_remove};
    const h2_pal_fs_api_t fs = {.vtable = &fs_vtable};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = idle_power_running,
    };
    const h2_pal_power_api_t power = {.user = &context, .vtable = &power_vtable};
    const h2_pal_disk_vtable_t disk_vtable = {
        .get_partition = progress_disk_get_partition,
        .read = progress_disk_read,
    };
    const h2_pal_disk_api_t disk = {
        .user = &disk_context,
        .vtable = &disk_vtable,
    };

    strcpy(context.upgrade_step, "canonical_confirm");
    record.format = 1u;
    record.phase = H2_LOADER_UPGRADE_PHASE_CANONICAL_PENDING;
    strcpy(record.package_sha256, TEST_AB_SHA256);
    record.candidate.format = 1u;
    record.candidate.role = H2_LOADER_IMAGE_ROLE_H2LOADER;
    strcpy(record.candidate.board, "test");
    strcpy(record.candidate.target, "host");
    strcpy(record.candidate.version, "completed-upgrade");
    record.candidate.image_size = sizeof(disk_context.source);
    fill_sha256(record.candidate.image_sha256, '0');
    record.canonical_partition = 1u;
    record.trial_partition = 2u;
    assert(h2_loader_upgrade_record_encode(
               &record,
               context.upgrade_record,
               sizeof(context.upgrade_record),
               &context.upgrade_record_len) == H2_PAL_OK);

    loader.config.package.disk = &disk;
    loader.config.package.fs = &fs;
    loader.config.package.package_path = H2_LOADER_DEFAULT_PACKAGE_PATH;
    loader.config.package.allocator = &mem;
    loader.config.package.digest = (h2_loader_digest_api_t){
        .start = test_digest_start,
        .update = test_digest_update,
        .finish = test_digest_finish,
        .abort = disk_digest_abort,
    };
    loader.package.config = loader.config.package;
    loader.config.pref = &pref;
    loader.config.power = &power;
    loader.config.board = "test";
    loader.config.target = "host";
    loader.config.h2loader_partition_id = 1u;
    loader.config.app_partition_id = 2u;
    loader.config.active_identity = record.candidate;
    loader.config.confirm_active_image = idle_confirm_active;
    loader.config.confirm_user = &context;

    assert(h2_loader_startup(&loader, &action) == H2_PAL_OK);
    assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
    assert(context.confirms == 1);
    assert(context.upgrade_step_present == 0);
    assert(context.upgrade_step_removes == 1);
    assert(h2_loader_upgrade_record_decode(
               context.upgrade_record,
               context.upgrade_record_len,
               &completed) == H2_PAL_OK);
    assert(completed.phase == H2_LOADER_UPGRADE_PHASE_IDLE);
    assert(completed.last_result == H2_PAL_OK);
    assert(strcmp(completed.package_sha256, TEST_AB_SHA256) == 0);
}

static void test_successful_upgrade_recovery_clears_staged_candidate(void) {
    stage_test_fixture_t fixture;
    disk_progress_context_t disk_context = {0};
    h2_loader_upgrade_record_t record = {0};
    h2_loader_upgrade_record_t completed = {0};
    h2_loader_startup_action_t action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
    const h2_pal_disk_vtable_t disk_vtable = {
        .get_partition = progress_disk_get_partition,
        .read = progress_disk_read,
    };
    const h2_pal_disk_api_t disk = {
        .user = &disk_context,
        .vtable = &disk_vtable,
    };

    stage_test_fixture_init(&fixture);
    fixture.context.package_exists = 1;
    fixture.context.package_size = 8u;
    fixture.context.staged_valid = 1;
    strcpy(fixture.context.staged_version, TEST_AB_SHA256);
    strcpy(fixture.context.staged_checksum, TEST_AB_SHA256);
    fixture.context.staged_size = 8u;

    record.format = 1u;
    record.phase = H2_LOADER_UPGRADE_PHASE_CANONICAL_PENDING;
    strcpy(record.package_sha256, TEST_AB_SHA256);
    record.candidate.format = 1u;
    record.candidate.role = H2_LOADER_IMAGE_ROLE_H2LOADER;
    strcpy(record.candidate.board, "test");
    strcpy(record.candidate.target, "host");
    strcpy(record.candidate.version, "completed-upgrade");
    record.candidate.image_size = sizeof(disk_context.source);
    fill_sha256(record.candidate.image_sha256, '0');
    record.canonical_partition = 1u;
    record.trial_partition = 2u;
    assert(h2_loader_upgrade_record_encode(
               &record,
               fixture.context.upgrade_record,
               sizeof(fixture.context.upgrade_record),
               &fixture.context.upgrade_record_len) == H2_PAL_OK);

    fixture.loader.config.package.disk = &disk;
    fixture.loader.config.package.digest = (h2_loader_digest_api_t){
        .start = test_digest_start,
        .update = test_digest_update,
        .finish = test_digest_finish,
        .abort = disk_digest_abort,
    };
    fixture.loader.package.config = fixture.loader.config.package;
    fixture.loader.config.h2loader_partition_id = 1u;
    fixture.loader.config.app_partition_id = 2u;
    fixture.loader.config.active_identity = record.candidate;

    assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
    assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
    assert(fixture.context.package_exists == 0);
    assert(fixture.context.staged_valid == 0);
    assert(fixture.context.install_state == H2_LOADER_INSTALL_STATE_IDLE);
    assert(fixture.context.boot_intent == H2_LOADER_BOOT_INTENT_H2LOADER);
    assert(h2_loader_upgrade_record_decode(
               fixture.context.upgrade_record,
               fixture.context.upgrade_record_len,
               &completed) == H2_PAL_OK);
    assert(completed.phase == H2_LOADER_UPGRADE_PHASE_IDLE);
    assert(completed.last_result == H2_PAL_OK);
    assert(strcmp(completed.package_sha256, TEST_AB_SHA256) == 0);
}

static void test_command_registration_and_help(void) {
    static const char input[] = "h2loader help\nh2loader\n";
    static const char expected[] =
        "h2loader <help|status|stats|memory|wifi|stage|upgrade|reboot [app|loader]|hold|coredump>\n"
        "usage: h2loader <help|status|stats|memory|wifi|stage|upgrade|reboot [app|loader]|hold|coredump>\n";
    command_test_io_t io;
    h2_loader_t loader;
    h2_loader_command_t command;

    memset(&io, 0, sizeof(io));
    memset(&loader, 0, sizeof(loader));
    io.input = input;
    io.input_len = sizeof(input) - 1u;
    assert(command_test_init(&command, &loader, &io) == H2_PAL_OK);
    assert(command.command.definition_count == H2_LOADER_COMMAND_DEFINITION_CAPACITY);
    assert(h2_loader_command_poll(&command, 10u) == H2_PAL_OK);
    assert(h2_loader_command_poll(&command, 10u) == H2_PAL_OK);
    assert(io.flushes == 2u);
    assert(io.output_len == sizeof(expected) - 1u);
    assert(memcmp(io.output, expected, sizeof(expected) - 1u) == 0);
}

static void test_status_command_writes_complete_line_atomically(void) {
    static const char input[] = "h2loader status\n";
    idle_trial_context_t context = {.running_partition = 1u};
    command_test_io_t io = {
        .input = input,
        .input_len = sizeof(input) - 1u,
    };
    h2_loader_t loader = {0};
    h2_loader_command_t command;
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = idle_power_running,
    };
    const h2_pal_power_api_t power = {.user = &context, .vtable = &power_vtable};
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = idle_mem_alloc,
        .free = idle_mem_free,
    };
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};

    loader.config.pref = &pref;
    loader.config.power = &power;
    loader.config.package.allocator = &mem;
    loader.config.hardware_capabilities = H2_LOADER_CAPABILITY_UART;
    assert(command_test_init(&command, &loader, &io) == H2_PAL_OK);
    assert(h2_loader_command_poll(&command, 10u) == H2_PAL_OK);
    assert(io.writes == 1u);
    assert(io.output_len > 0u);
    assert(io.output[io.output_len - 1u] == '\n');
    assert(memcmp(io.output, "H2_LOADER_STATUS ",
                  strlen("H2_LOADER_STATUS ")) == 0);
}

static void test_memory_command_preserves_stats_identity_alias(void) {
    static const char input[] = "h2loader memory\n";
    static const char expected[] =
        "H2_LOADER_MEMORY result=OK internal_total=1000 internal_free=600 "
        "internal_min_free=500 internal_largest=400 iram_total=300 iram_free=200 "
        "iram_min_free=150 iram_largest=100 psram_total=8000 psram_free=7000 "
        "psram_min_free=6500 psram_largest=6000\n";
    command_test_io_t io = {
        .input = input,
        .input_len = sizeof(input) - 1u,
    };
    h2_loader_t loader = {0};
    h2_loader_command_t command;
    int found_stats = 0;

    assert(command_test_init(&command, &loader, &io) == H2_PAL_OK);
    assert(h2_loader_command_poll(&command, 10u) == H2_PAL_OK);
    assert(io.output_len == sizeof(expected) - 1u);
    assert(memcmp(io.output, expected, sizeof(expected) - 1u) == 0);
    for (size_t i = 0u; i < command.command.definition_count; ++i) {
        if (strcmp(command.definitions[i].path, "h2loader stats") == 0) {
            found_stats = 1;
        }
    }
    assert(found_stats == 1);
}

static void test_mfg_gate_keeps_canonical_loader_in_command_mode(void) {
    idle_trial_context_t context = {.running_partition = 1u};
    h2_loader_t loader = {0};
    h2_loader_startup_action_t action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
    const h2_pal_mem_vtable_t mem_vtable = {0};
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = idle_power_running,
        .set_next_boot_partition = idle_power_select,
        .reboot = idle_power_reboot,
    };
    const h2_pal_power_api_t power = {.user = &context, .vtable = &power_vtable};

    loader.config.package.allocator = &mem;
    loader.config.pref = &pref;
    loader.config.power = &power;
    loader.config.h2loader_partition_id = 1u;
    loader.config.app_partition_id = 2u;
    loader.config.mfg_required_total = 8u;
    loader.config.disruptive_user = &context;
    loader.config.before_disruptive = idle_before_disruptive;
    assert(h2_loader_startup(&loader, &action) == H2_PAL_OK);
    assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
    assert(h2_loader_boot_app(&loader) == H2_PAL_ERR_INVALID_STATE);
    assert(context.disruptive_calls == 0);
    assert(context.boot_selections == 0);
    assert(context.reboots == 0);
    assert(h2_loader_set_mfg_gate_bypass(NULL, 1) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_loader_set_mfg_gate_bypass(&loader, 2) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_loader_set_mfg_gate_bypass(&loader, 1) == H2_PAL_OK);
    assert(h2_loader_boot_app(&loader) == H2_PAL_OK);
    assert(context.disruptive_calls == 1);
    assert(context.boot_selections == 1);
    assert(context.reboots == 1);
    assert(h2_loader_set_mfg_gate_bypass(&loader, 0) == H2_PAL_OK);
    assert(h2_loader_boot_app(&loader) == H2_PAL_ERR_INVALID_STATE);
}

static void test_durable_install_request_precedes_fresh_mfg_gate(void) {
    stage_test_fixture_t fixture;
    h2_loader_startup_action_t action =
        H2_LOADER_STARTUP_ACTION_REBOOTING_APP;

    stage_test_fixture_init(&fixture);
    fixture.context.install_state =
        H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED;
    fixture.context.manual_hold = 1;
    fixture.loader.config.mfg_required_total = H2_LOADER_MFG_STEP_TOTAL;

    assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
    assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
    assert(h2_loader_boot_app(&fixture.loader) != H2_PAL_ERR_INVALID_STATE);
    assert(h2_loader_set_mfg_gate_bypass(&fixture.loader, 0) == H2_PAL_OK);
    assert(h2_loader_boot_app(&fixture.loader) == H2_PAL_ERR_INVALID_STATE);
}

static void test_forced_command_mode_confirms_active_image(void) {
    idle_trial_context_t context = {.running_partition = 1u};
    h2_loader_t loader = {0};
    h2_loader_startup_action_t action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
    const h2_pal_mem_vtable_t mem_vtable = {0};
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_pal_pref_vtable_t pref_vtable = {.open = idle_pref_open};
    const h2_pal_pref_api_t pref = {.user = &context, .vtable = &pref_vtable};
    const h2_pal_power_vtable_t power_vtable = {
        .get_running_boot_partition = idle_power_running,
        .set_next_boot_partition = idle_power_select,
        .reboot = idle_power_reboot,
    };
    const h2_pal_power_api_t power = {.user = &context, .vtable = &power_vtable};

    loader.config.package.allocator = &mem;
    loader.config.pref = &pref;
    loader.config.power = &power;
    loader.config.h2loader_partition_id = 1u;
    loader.config.app_partition_id = 2u;
    loader.config.confirm_user = &context;
    loader.config.confirm_active_image = idle_confirm_active;
    loader.force_command_mode = 1;

    assert(h2_loader_startup(&loader, &action) == H2_PAL_OK);
    assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
    assert(context.confirms == 1);
    assert(context.boot_selections == 0);
    assert(context.reboots == 0);

    context.confirm_result = H2_PAL_ERR_IO;
    assert(h2_loader_startup(&loader, &action) == H2_PAL_ERR_IO);
    assert(context.confirms == 2);
}

static void test_command_reinit_discards_partial_old_session_input(void) {
    static const char partial[] = "h2loader hel";
    static const char replacement[] = "h2loader help\n";
    static const h2_command_io_vtable_t io_vtable = {
        .read = command_test_read,
        .write = command_test_write,
        .flush = command_test_flush,
    };
    static const h2_pal_fs_vtable_t fs_vtable = {0};
    static const h2_pal_http_vtable_t http_vtable = {0};
    static const h2_pal_wifi_sta_vtable_t wifi_vtable = {0};
    static const h2_pal_disk_vtable_t disk_vtable = {0};
    command_test_io_t io;
    h2_loader_t loader;
    h2_loader_command_t command;
    h2_loader_command_config_t config;
    h2_pal_fs_api_t fs = {.vtable = &fs_vtable};
    h2_pal_http_api_t http = {.vtable = &http_vtable};
    h2_pal_wifi_sta_api_t wifi = {.vtable = &wifi_vtable};
    h2_pal_disk_api_t disk = {.vtable = &disk_vtable};

    memset(&io, 0, sizeof(io));
    memset(&loader, 0, sizeof(loader));
    memset(&config, 0, sizeof(config));
    io.input = partial;
    io.input_len = sizeof(partial) - 1u;
    io.exhausted_result = H2_PAL_ERR_CLOSED;
    config.loader = &loader;
    config.fs = &fs;
    config.http = &http;
    config.wifi = &wifi;
    config.disk = &disk;
    config.digest.start = command_test_digest_start;
    config.digest.update = command_test_digest_update;
    config.digest.finish = command_test_digest_finish;
    config.now_ms = command_test_now_ms;
    config.sleep_ms = command_test_sleep_ms;
    config.io.user = &io;
    config.io.vtable = &io_vtable;
    assert(h2_loader_command_init(&command, &config) == H2_PAL_OK);
    assert(h2_loader_command_poll(&command, 10u) == H2_PAL_ERR_CLOSED);
    assert(command.command.input_len == sizeof(partial) - 1u);

    io.input = replacement;
    io.input_len = sizeof(replacement) - 1u;
    io.input_offset = 0u;
    io.exhausted_result = H2_PAL_OK;
    assert(h2_loader_command_init(&command, &config) == H2_PAL_OK);
    assert(command.command.input_len == 0u);
    assert(h2_loader_command_poll(&command, 10u) == H2_PAL_OK);
    assert(memcmp(io.output, "h2loader <help|", 15u) == 0);
}

static void test_stage_close_removes_only_incomplete_tmp(void) {
    static const char sha256[] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    static const h2_command_io_vtable_t io_vtable = {
        .read = command_test_read,
        .write = command_test_write,
        .flush = command_test_flush,
    };
    char input[160];
    command_test_io_t io;
    stage_test_fixture_t fixture;
    h2_loader_command_t command;
    h2_loader_command_config_t config;

    assert(strlen(sha256) == 64u);
    int input_len = snprintf(input, sizeof(input), "h2loader stage 8 %s\nabcd", sha256);
    assert(input_len > 0 && (size_t)input_len < sizeof(input));
    memset(&io, 0, sizeof(io));
    stage_test_fixture_init(&fixture);
    fixture.context.capacity = 8u;
    fixture.context.package_exists = 1;
    fixture.context.package_size = 8u;
    fixture.context.staged_valid = 1;
    strcpy(fixture.context.staged_version, "old");
    strcpy(fixture.context.staged_checksum, TEST_AB_SHA256);
    fixture.context.staged_size = 8u;
    memset(&config, 0, sizeof(config));
    io.input = input;
    io.input_len = (size_t)input_len;
    io.exhausted_result = H2_PAL_ERR_CLOSED;
    config.loader = &fixture.loader;
    config.fs = &fixture.fs;
    config.http = &fixture.http;
    config.wifi = &fixture.wifi;
    config.disk = &fixture.disk;
    config.digest.start = command_test_digest_start;
    config.digest.update = command_test_digest_update;
    config.digest.finish = command_test_digest_finish;
    config.now_ms = command_test_now_ms;
    config.sleep_ms = command_test_sleep_ms;
    config.io.user = &io;
    config.io.vtable = &io_vtable;
    assert(h2_loader_command_init(&command, &config) == H2_PAL_OK);
    assert(h2_loader_command_poll(&command, 10u) == H2_PAL_ERR_CLOSED);
    assert(fixture.context.opens == 1u);
    assert(fixture.context.closes == 1u);
    assert(fixture.context.removes >= 4u);
    assert(fixture.context.package_present_when_opened == 0);
    assert(fixture.context.package_exists == 0);
    assert(fixture.context.temporary_exists == 0);
    assert(fixture.context.staged_valid == 0);
    assert(fixture.context.install_state == H2_LOADER_INSTALL_STATE_CONFIRMED);
    assert(fixture.context.boot_intent == H2_LOADER_BOOT_INTENT_APP);
}

static void test_url_stage_discards_old_candidate_before_wifi(void) {
    static const char *const argv[] = {
        "h2loader",
        "stage",
        "url",
        "http://127.0.0.1/update.tar.zlib",
        "8",
        TEST_AB_SHA256,
    };
    static const h2_command_io_vtable_t io_vtable = {
        .read = command_test_read,
        .write = command_test_write,
        .flush = command_test_flush,
    };
    stage_test_fixture_t fixture;
    command_test_io_t io = {0};
    h2_loader_command_t command;
    h2_loader_command_config_t config;

    stage_test_fixture_init(&fixture);
    fixture.context.package_exists = 1;
    fixture.context.package_size = 8u;
    fixture.context.staged_valid = 1;
    strcpy(fixture.context.staged_version, "old");
    strcpy(fixture.context.staged_checksum, TEST_CD_SHA256);
    fixture.context.staged_size = 8u;
    fixture.context.install_state =
        H2_LOADER_INSTALL_STATE_RETURN_REQUESTED;
    fixture.context.boot_intent = H2_LOADER_BOOT_INTENT_H2LOADER;
    fixture.context.manual_hold = 1;
    fixture.context.last_result = H2_PAL_ERR_TIMEOUT;

    memset(&config, 0, sizeof(config));
    config.loader = &fixture.loader;
    config.fs = &fixture.fs;
    config.http = &fixture.http;
    config.wifi = &fixture.wifi;
    config.disk = &fixture.disk;
    config.digest.start = command_test_digest_start;
    config.digest.update = command_test_digest_update;
    config.digest.finish = command_test_digest_finish;
    config.now_ms = command_test_now_ms;
    config.sleep_ms = command_test_sleep_ms;
    config.io.user = &io;
    config.io.vtable = &io_vtable;

    assert(h2_loader_command_init(&command, &config) == H2_PAL_OK);
    assert(h2_loader_command_execute(
        &command, sizeof(argv) / sizeof(argv[0]), argv) ==
        H2_PAL_ERR_UNAVAILABLE);
    assert(fixture.context.wifi_status_calls == 2u);
    assert(fixture.context.package_present_when_wifi_checked == 0);
    assert(fixture.context.package_exists == 0);
    assert(fixture.context.staged_valid == 0);
    assert(fixture.context.install_state ==
        H2_LOADER_INSTALL_STATE_RETURN_REQUESTED);
    assert(fixture.context.boot_intent ==
        H2_LOADER_BOOT_INTENT_H2LOADER);
    assert(fixture.context.manual_hold == 1);
    assert(fixture.context.last_result == H2_PAL_ERR_TIMEOUT);
}

static void test_stage_publication_preserves_app_lifecycle(void) {
    stage_test_fixture_t fixture;

    stage_test_fixture_init(&fixture);
    fixture.context.install_state =
        H2_LOADER_INSTALL_STATE_RETURN_REQUESTED;
    fixture.context.boot_intent = H2_LOADER_BOOT_INTENT_H2LOADER;
    fixture.context.manual_hold = 1;
    fixture.context.last_result = H2_PAL_ERR_WRITE;
    fixture.context.package_exists = 1;
    fixture.context.package_size = 7u;

    assert(h2_loader_publish_stage(
        &fixture.loader, 7u, TEST_AB_SHA256) == H2_PAL_OK);
    assert(fixture.context.staged_valid == 1);
    assert(strcmp(fixture.context.staged_checksum, TEST_AB_SHA256) == 0);
    assert(fixture.context.staged_size == 7u);
    assert(fixture.context.publish_marker_present == 1);
    assert(fixture.context.publish_committed == 1);
    assert(fixture.context.install_state ==
        H2_LOADER_INSTALL_STATE_RETURN_REQUESTED);
    assert(fixture.context.boot_intent ==
        H2_LOADER_BOOT_INTENT_H2LOADER);
    assert(fixture.context.app_confirmed == 1);
    assert(fixture.context.manual_hold == 1);
    assert(fixture.context.last_result == H2_PAL_ERR_WRITE);

    fixture.context.package_exists = 1;
    fixture.context.package_size = 9u;
    fixture.context.staged_valid = 0;
    fixture.context.staged_version[0] = '\0';
    fixture.context.staged_checksum[0] = '\0';
    fixture.context.staged_size = 0u;
    fixture.context.publish_marker_present = 1;
    fixture.context.publish_committed = 0;
    fixture.context.pref_set_error = H2_PAL_ERR_WRITE;
    assert(h2_loader_publish_stage(
        &fixture.loader, 9u, TEST_CD_SHA256) == H2_PAL_ERR_WRITE);
    assert(fixture.context.package_exists == 0);
    assert(fixture.context.staged_valid == 0);
    assert(fixture.context.install_state ==
        H2_LOADER_INSTALL_STATE_RETURN_REQUESTED);
    assert(fixture.context.boot_intent ==
        H2_LOADER_BOOT_INTENT_H2LOADER);
}

static void test_stage_replacement_normalizes_only_legacy_staged_state(void) {
    stage_test_fixture_t fixture;

    stage_test_fixture_init(&fixture);
    fixture.context.install_state = H2_LOADER_INSTALL_STATE_STAGED;
    fixture.context.boot_intent = H2_LOADER_BOOT_INTENT_H2LOADER;
    fixture.context.package_exists = 1;
    fixture.context.package_size = 8u;
    fixture.context.staged_valid = 1;
    strcpy(fixture.context.staged_version, "old");
    strcpy(fixture.context.staged_checksum, TEST_AB_SHA256);
    fixture.context.staged_size = 8u;
    assert(h2_loader_begin_stage_replacement(
        &fixture.loader,
        "/dl/update.tar.zlib.tmp",
        "/dl/update.tar.zlib.prev") == H2_PAL_OK);
    assert(fixture.context.install_state ==
        H2_LOADER_INSTALL_STATE_CONFIRMED);
    assert(fixture.context.boot_intent == H2_LOADER_BOOT_INTENT_APP);
    assert(fixture.context.staged_valid == 0);

    stage_test_fixture_init(&fixture);
    fixture.context.install_state = H2_LOADER_INSTALL_STATE_STAGED;
    fixture.context.boot_intent = H2_LOADER_BOOT_INTENT_H2LOADER;
    fixture.context.installed_valid = 0;
    fixture.context.app_confirmed = 0;
    assert(h2_loader_begin_stage_replacement(
        &fixture.loader,
        "/dl/update.tar.zlib.tmp",
        "/dl/update.tar.zlib.prev") == H2_PAL_OK);
    assert(fixture.context.install_state == H2_LOADER_INSTALL_STATE_IDLE);
    assert(fixture.context.boot_intent ==
        H2_LOADER_BOOT_INTENT_H2LOADER);

    stage_test_fixture_init(&fixture);
    fixture.context.install_state = H2_LOADER_INSTALL_STATE_STAGED;
    fixture.context.boot_intent = H2_LOADER_BOOT_INTENT_H2LOADER;
    fixture.context.app_confirmed = 0;
    assert(h2_loader_begin_stage_replacement(
        &fixture.loader,
        "/dl/update.tar.zlib.tmp",
        "/dl/update.tar.zlib.prev") == H2_PAL_OK);
    assert(fixture.context.install_state ==
        H2_LOADER_INSTALL_STATE_MAIN_FAILED);
    assert(fixture.context.boot_intent ==
        H2_LOADER_BOOT_INTENT_H2LOADER);
    assert(h2_loader_begin_stage_replacement(
        &fixture.loader,
        "/dl/update.tar.zlib.tmp",
        "/dl/update.tar.zlib.prev") == H2_PAL_OK);
    assert(fixture.context.install_state ==
        H2_LOADER_INSTALL_STATE_MAIN_FAILED);
}

static void test_stage_replacement_cleans_files_after_preference_failure(void) {
    stage_test_fixture_t fixture;

    stage_test_fixture_init(&fixture);
    fixture.context.package_exists = 1;
    fixture.context.package_size = 8u;
    fixture.context.temporary_exists = 1;
    fixture.context.temporary_size = 4u;
    fixture.context.previous_exists = 1;
    fixture.context.previous_size = 8u;
    fixture.context.staged_valid = 1;
    strcpy(fixture.context.staged_version, "old");
    strcpy(fixture.context.staged_checksum, TEST_AB_SHA256);
    fixture.context.staged_size = 8u;
    fixture.context.pref_set_error = H2_PAL_ERR_WRITE;

    assert(h2_loader_begin_stage_replacement(
        &fixture.loader,
        "/dl/update.tar.zlib.tmp",
        "/dl/update.tar.zlib.prev") == H2_PAL_ERR_WRITE);
    assert(fixture.context.package_exists == 0);
    assert(fixture.context.temporary_exists == 0);
    assert(fixture.context.previous_exists == 0);
    assert(fixture.context.staged_valid == 0);
    assert(fixture.context.opens == 0u);
}

static void test_startup_normalizes_legacy_staged_without_installing(void) {
    stage_test_fixture_t fixture;
    h2_loader_startup_action_t action =
        H2_LOADER_STARTUP_ACTION_REBOOTING_APP;

    stage_test_fixture_init(&fixture);
    fixture.context.install_state = H2_LOADER_INSTALL_STATE_STAGED;
    fixture.context.boot_intent = H2_LOADER_BOOT_INTENT_H2LOADER;
    fixture.context.manual_hold = 1;
    fixture.context.package_exists = 1;
    fixture.context.package_size = 8u;
    fixture.context.staged_valid = 1;
    strcpy(fixture.context.staged_version, "candidate");
    strcpy(fixture.context.staged_checksum, TEST_AB_SHA256);
    fixture.context.staged_size = 8u;
    fixture.context.publish_marker_present = 1;
    fixture.context.publish_committed = 1;

    assert(h2_loader_startup(&fixture.loader, &action) == H2_PAL_OK);
    assert(action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE);
    assert(fixture.context.install_state ==
        H2_LOADER_INSTALL_STATE_CONFIRMED);
    assert(fixture.context.boot_intent == H2_LOADER_BOOT_INTENT_APP);
    assert(fixture.context.package_exists == 1);
    assert(fixture.context.staged_valid == 1);
    assert(fixture.context.removes == 0u);
}

static void test_stage_recovery_never_restores_discarded_candidate(void) {
    stage_test_fixture_t fixture;

    stage_test_fixture_init(&fixture);
    fixture.context.package_exists = 1;
    fixture.context.package_size = 7u;
    fixture.context.temporary_exists = 1;
    fixture.context.temporary_size = 5u;
    fixture.context.previous_exists = 1;
    fixture.context.previous_size = 8u;
    fixture.context.staged_valid = 1;
    strcpy(fixture.context.staged_version, "old");
    strcpy(fixture.context.staged_checksum, TEST_AB_SHA256);
    fixture.context.staged_size = 8u;
    fixture.context.publish_marker_present = 1;
    fixture.context.publish_committed = 0;
    assert(h2_loader_package_recover_publish(
        &fixture.fs,
        &fixture.pref,
        "/dl/update.tar.zlib",
        "/dl/update.tar.zlib.prev") == H2_PAL_OK);
    assert(fixture.context.package_exists == 0);
    assert(fixture.context.temporary_exists == 0);
    assert(fixture.context.previous_exists == 0);
    assert(fixture.context.staged_valid == 0);
    assert(fixture.context.publish_marker_present == 0);

    stage_test_fixture_init(&fixture);
    fixture.context.package_exists = 1;
    fixture.context.package_size = 8u;
    fixture.context.staged_valid = 1;
    strcpy(fixture.context.staged_version, "current");
    strcpy(fixture.context.staged_checksum, TEST_CD_SHA256);
    fixture.context.staged_size = 8u;
    fixture.context.publish_marker_present = 1;
    fixture.context.publish_committed = 1;
    assert(h2_loader_package_recover_publish(
        &fixture.fs,
        &fixture.pref,
        "/dl/update.tar.zlib",
        "/dl/update.tar.zlib.prev") == H2_PAL_OK);
    assert(fixture.context.package_exists == 1);
    assert(fixture.context.staged_valid == 1);
    assert(fixture.context.publish_committed == 1);

    stage_test_fixture_init(&fixture);
    fixture.context.package_exists = 1;
    fixture.context.package_size = 7u;
    fixture.context.previous_exists = 1;
    fixture.context.previous_size = 8u;
    fixture.context.staged_valid = 1;
    strcpy(fixture.context.staged_version, "current");
    strcpy(fixture.context.staged_checksum, TEST_CD_SHA256);
    fixture.context.staged_size = 7u;
    fixture.context.publish_marker_present = 1;
    fixture.context.publish_committed = 1;
    assert(h2_loader_package_recover_publish(
        &fixture.fs,
        &fixture.pref,
        "/dl/update.tar.zlib",
        "/dl/update.tar.zlib.prev") == H2_PAL_OK);
    assert(fixture.context.package_exists == 1);
    assert(fixture.context.package_size == 7u);
    assert(fixture.context.previous_exists == 0);
    assert(fixture.context.staged_valid == 1);
    assert(strcmp(fixture.context.staged_checksum, TEST_CD_SHA256) == 0);
    assert(fixture.context.staged_size == 7u);
    assert(fixture.context.publish_marker_present == 1);
    assert(fixture.context.publish_committed == 1);

    stage_test_fixture_init(&fixture);
    fixture.context.package_exists = 1;
    fixture.context.package_is_dir = 1;
    fixture.context.temporary_exists = 1;
    fixture.context.temporary_is_dir = 1;
    fixture.context.previous_exists = 1;
    fixture.context.previous_is_dir = 1;
    fixture.context.staged_valid = 1;
    strcpy(fixture.context.staged_version, "invalid-directory");
    strcpy(fixture.context.staged_checksum, TEST_AB_SHA256);
    fixture.context.staged_size = 8u;
    fixture.context.publish_marker_present = 1;
    fixture.context.publish_committed = 1;
    assert(h2_loader_package_recover_publish(
        &fixture.fs,
        &fixture.pref,
        "/dl/update.tar.zlib",
        "/dl/update.tar.zlib.prev") == H2_PAL_OK);
    assert(fixture.context.package_exists == 0);
    assert(fixture.context.temporary_exists == 0);
    assert(fixture.context.previous_exists == 0);
    assert(fixture.context.staged_valid == 0);
    assert(fixture.context.publish_marker_present == 0);
}

typedef struct app_coredump_test_context {
    uint8_t storage[32];
    char output[512];
    size_t output_len;
    size_t erases;
} app_coredump_test_context_t;

typedef struct app_return_test_context {
    idle_trial_context_t lifecycle;
    struct h2_pal_task task;
    const char *input;
    size_t input_offset;
    char output[256];
    size_t output_len;
    int write_result;
} app_return_test_context_t;

static int app_return_read_byte(void *user, uint32_t timeout_ms) {
    app_return_test_context_t *context = user;
    (void)timeout_ms;
    if (context->input[context->input_offset] == '\0') {
        return H2_LOADER_APP_CLIENT_SESSION_CLOSED;
    }
    return (unsigned char)context->input[context->input_offset++];
}

static int app_return_write(void *user, const char *data, size_t len) {
    app_return_test_context_t *context = user;
    if (context->write_result != H2_PAL_OK) {
        return context->write_result;
    }
    assert(context->output_len + len < sizeof(context->output));
    if (len >= strlen("H2_LOADER_ROLLBACK") &&
        memcmp(data, "H2_LOADER_ROLLBACK", strlen("H2_LOADER_ROLLBACK")) == 0) {
        assert(context->lifecycle.commits == 3);
        assert(context->lifecycle.boot_selections == 1);
        assert(context->lifecycle.reboots == 0);
    }
    memcpy(context->output + context->output_len, data, len);
    context->output_len += len;
    context->output[context->output_len] = '\0';
    return H2_PAL_OK;
}

static int app_return_task_start(
    void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task) {
    app_return_test_context_t *context = user;
    assert(options != NULL && options->min_stack_size >= 8192u);
    assert(entry != NULL && out_task != NULL);
    entry(ctx);
    context->task.completed = 1;
    *out_task = &context->task;
    return H2_PAL_OK;
}

static int app_return_task_join(void *user, h2_pal_task_t *task) {
    app_return_test_context_t *context = user;
    assert(task == &context->task && context->task.completed != 0);
    return H2_PAL_OK;
}

static h2_pal_result_t app_coredump_get_partition(
    void *user,
    uint32_t partition_id,
    h2_pal_disk_partition_t *out_partition) {
    app_coredump_test_context_t *context = user;

    if (context == NULL || partition_id != 7u || out_partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    memset(out_partition, 0, sizeof(*out_partition));
    out_partition->id = partition_id;
    out_partition->size = sizeof(context->storage);
    (void)snprintf(out_partition->name, sizeof(out_partition->name), "coredump");
    return H2_PAL_OK;
}

static h2_pal_result_t app_coredump_read(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    void *data,
    size_t len) {
    app_coredump_test_context_t *context = user;

    if (context == NULL || partition_id != 7u || data == NULL ||
        offset > sizeof(context->storage) ||
        len > sizeof(context->storage) - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(data, context->storage + offset, len);
    return H2_PAL_OK;
}

static h2_pal_result_t app_coredump_erase(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    uint64_t len) {
    app_coredump_test_context_t *context = user;

    if (context == NULL || partition_id != 7u || offset != 0u ||
        len != sizeof(context->storage)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(context->storage, 0xff, sizeof(context->storage));
    context->erases += 1u;
    return H2_PAL_OK;
}

static int app_coredump_write(void *user, const char *data, size_t len) {
    app_coredump_test_context_t *context = user;

    assert(context != NULL);
    assert(context->output_len + len < sizeof(context->output));
    memcpy(context->output + context->output_len, data, len);
    context->output_len += len;
    context->output[context->output_len] = '\0';
    return H2_PAL_OK;
}

static void test_app_client_requires_stable_hardware_capabilities(void) {
    static const h2_pal_pref_vtable_t pref_vtable = {0};
    static const h2_pal_power_vtable_t power_vtable = {0};
    static const h2_pal_mem_vtable_t mem_vtable = {0};
    static const h2_pal_disk_vtable_t disk_vtable = {0};
    const h2_pal_pref_api_t pref = {.vtable = &pref_vtable};
    const h2_pal_power_api_t power = {.vtable = &power_vtable};
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_pal_disk_api_t disk = {.vtable = &disk_vtable};
    h2_loader_app_client_config_t config = {
        .pref = &pref,
        .power = &power,
        .allocator = &mem,
    };
    h2_loader_app_client_t client;

    assert(h2_loader_app_client_init(&client, &config) ==
           H2_PAL_ERR_INVALID_ARG);
    config.hardware_capabilities = H2_LOADER_CAPABILITY_UART;
    config.disk = &disk;
    assert(h2_loader_app_client_init(&client, &config) == H2_PAL_OK);
    assert(client.config.hardware_capabilities == H2_LOADER_CAPABILITY_UART);
}

static void test_app_client_coredump_uses_caller_output(void) {
    static const h2_pal_disk_vtable_t disk_vtable = {
        .get_partition = app_coredump_get_partition,
        .read = app_coredump_read,
        .erase = app_coredump_erase,
    };
    app_coredump_test_context_t context;
    h2_pal_disk_api_t disk = {.user = &context, .vtable = &disk_vtable};
    h2_loader_app_client_t client;

    memset(&context, 0, sizeof(context));
    memset(&client, 0, sizeof(client));
    memset(context.storage, 0xff, sizeof(context.storage));
    client.config.disk = &disk;
    client.config.coredump_partition_id = 7u;

    assert(h2_loader_app_client_coredump(
               &client, NULL, &context, app_coredump_write) == H2_PAL_OK);
    assert(strstr(context.output,
               "H2_LOADER_COREDUMP_STATUS result=OK") != NULL);
    assert(strstr(context.output, "stored_bytes=0 blank=1") != NULL);

    context.output_len = 0u;
    context.output[0] = '\0';
    assert(h2_loader_app_client_coredump(
               &client, "erase", &context, app_coredump_write) == H2_PAL_OK);
    assert(context.erases == 1u);
    assert(strstr(context.output,
               "H2_LOADER_COREDUMP_ERASE result=OK") != NULL);
}

static void test_app_return_console_prepares_before_success(void) {
    static const h2_pal_pref_vtable_t pref_vtable = {
        .open = idle_pref_open,
    };
    static const h2_pal_power_vtable_t power_vtable = {
        .set_next_boot_partition = idle_power_select,
        .reboot = idle_power_reboot,
    };
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = idle_mem_alloc,
        .free = idle_mem_free,
    };
    static const h2_pal_task_vtable_t task_vtable = {
        .start = app_return_task_start,
        .join = app_return_task_join,
    };
    app_return_test_context_t context = {
        .input = "h2loader rollback\n",
    };
    const h2_pal_pref_api_t pref = {
        .user = &context.lifecycle,
        .vtable = &pref_vtable,
    };
    const h2_pal_power_api_t power = {
        .user = &context.lifecycle,
        .vtable = &power_vtable,
    };
    const h2_pal_mem_api_t mem = { .vtable = &mem_vtable };
    const h2_pal_task_api_t task = {
        .user = &context,
        .vtable = &task_vtable,
    };
    const h2_loader_app_client_config_t client_config = {
        .pref = &pref,
        .power = &power,
        .allocator = &mem,
        .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
        .h2loader_partition_id = 17u,
        .reboot_reason = 23u,
    };
    h2_loader_app_client_t client;
    const h2_loader_app_client_return_console_config_t console_config = {
        .client = &client,
        .task = &task,
        .read_user = &context,
        .read_byte = app_return_read_byte,
        .write_user = &context,
        .write = app_return_write,
    };

    assert(h2_loader_app_client_init(&client, &client_config) == H2_PAL_OK);
    assert(h2_loader_app_client_start_return_console(&console_config) == H2_PAL_OK);
    assert(context.lifecycle.selected_partition == 17u);
    assert(context.lifecycle.reboots == 1);
    assert(strstr(context.output, "H2_LOADER_ROLLBACK result=OK\n") != NULL);
    assert(h2_loader_app_client_join_return_console(&client) == H2_PAL_OK);
}

static void test_app_return_console_reboots_when_reply_fails(void) {
    static const h2_pal_pref_vtable_t pref_vtable = {
        .open = idle_pref_open,
    };
    static const h2_pal_power_vtable_t power_vtable = {
        .set_next_boot_partition = idle_power_select,
        .reboot = idle_power_reboot,
    };
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = idle_mem_alloc,
        .free = idle_mem_free,
    };
    static const h2_pal_task_vtable_t task_vtable = {
        .start = app_return_task_start,
        .join = app_return_task_join,
    };
    app_return_test_context_t context = {
        .input = "h2loader rollback\n",
        .write_result = H2_PAL_ERR_IO,
    };
    const h2_pal_pref_api_t pref = {
        .user = &context.lifecycle,
        .vtable = &pref_vtable,
    };
    const h2_pal_power_api_t power = {
        .user = &context.lifecycle,
        .vtable = &power_vtable,
    };
    const h2_pal_mem_api_t mem = { .vtable = &mem_vtable };
    const h2_pal_task_api_t task = {
        .user = &context,
        .vtable = &task_vtable,
    };
    const h2_loader_app_client_config_t client_config = {
        .pref = &pref,
        .power = &power,
        .allocator = &mem,
        .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
        .h2loader_partition_id = 17u,
        .reboot_reason = 23u,
    };
    h2_loader_app_client_t client;
    const h2_loader_app_client_return_console_config_t console_config = {
        .client = &client,
        .task = &task,
        .read_user = &context,
        .read_byte = app_return_read_byte,
        .write_user = &context,
        .write = app_return_write,
    };

    assert(h2_loader_app_client_init(&client, &client_config) == H2_PAL_OK);
    assert(h2_loader_app_client_start_return_console(&console_config) == H2_PAL_OK);
    assert(context.lifecycle.selected_partition == 17u);
    assert(context.lifecycle.reboots == 1);
    assert(context.output_len == 0u);
    assert(h2_loader_app_client_join_return_console(&client) == H2_PAL_OK);
}

static void test_ble_identity(void) {
    uint8_t payload[80];
    size_t payload_len = 0u;
    const uint8_t expected[] = {
        'H', '2', 'L', 'D', 1u, 0u, 0x05u, 0u, 0u, 0u, 6u,
        'd', 'e', 'v', 'k', 'i', 't',
    };

    assert(h2_loader_ble_encode_identity(
               H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_BLE, "devkit",
               payload, sizeof(payload), &payload_len) == H2_PAL_OK);
    assert(payload_len == sizeof(expected));
    assert(memcmp(payload, expected, sizeof(expected)) == 0);
    assert(h2_loader_ble_encode_identity(
               H2_LOADER_CAPABILITIES_ALL,
               "waveshare_esp32p4_wifi6_touch_lcd_4_3",
               payload, sizeof(payload), &payload_len) == H2_PAL_OK);
    const uint8_t expected_compact[] = {
        'H', '2', 'L', 'D', 2u, 0u, 0x07u, 0u, 0u, 0u,
        0x5eu, 0xe4u, 0x65u, 0xa0u, 0xc7u, 0xd3u, 0x26u, 0x2fu,
    };
    assert(payload_len == sizeof(expected_compact));
    assert(memcmp(payload, expected_compact, sizeof(expected_compact)) == 0);
    assert(h2_loader_ble_encode_identity(
               UINT32_C(1) << 3, "devkit",
               payload, sizeof(payload), &payload_len) == H2_PAL_ERR_INVALID_ARG);
    memset(payload, 0xa5, sizeof(payload));
    payload_len = 42u;
    assert(h2_loader_ble_encode_identity(
               H2_LOADER_CAPABILITY_UART, "devkit",
               payload, sizeof(expected) - 1u, &payload_len) == H2_PAL_ERR_INVALID_ARG);
    for (size_t i = 0u; i < sizeof(payload); ++i) {
        assert(payload[i] == 0xa5u);
    }
    assert(payload_len == 42u);
    assert(h2_loader_ble_encode_identity(
               H2_LOADER_CAPABILITIES_ALL,
               "waveshare_esp32p4_wifi6_touch_lcd_4_3",
               payload, sizeof(expected_compact) - 1u, &payload_len) == H2_PAL_ERR_INVALID_ARG);
    for (size_t i = 0u; i < sizeof(payload); ++i) {
        assert(payload[i] == 0xa5u);
    }
    assert(payload_len == 42u);
}

int main(void) {
    assert(h2_loader_install_requires_app_confirmation(1, 1));
    assert(h2_loader_install_requires_app_confirmation(1, 0));
    assert(!h2_loader_install_requires_app_confirmation(0, 1));
    assert(h2_loader_install_requires_app_confirmation(0, 0));
    test_install_plan_selects_changed_components_independently();
    test_install_rejects_changed_staged_archive();
    test_package_validation_recomputes_data_digest();
    test_package_validation_uses_configured_allocator();
    test_disk_writer_reports_image_progress();
    test_manifest_parser();
    test_upgrade_record_codec();
    test_mfg_record_round_trip_and_commit_failure();
    test_mfg_v3_record_rejects_wrong_length_or_status();
    test_mfg_legacy_record_reconstructs_passed_prefix();
    test_mfg_v2_record_migrates_masks_to_slots();
    test_mfg_acceptance_revision_invalidates_obsolete_pass();
    test_mfg_summary_validation_and_status_format();
    test_mfg_handoff_pending_truth_table();
    test_status_format_fits_shared_line_capacity();
    test_command_availability_flags();
    test_command_availability_reflects_loader_state();
    test_hardware_capabilities_are_stable();
    test_loader_commands_follow_command_availability();
    test_loader_commands_recheck_availability_after_operation_lock();
    test_idle_trial_never_reboots_itself();
    test_confirmed_state_without_installed_identity_stays_in_command_mode();
    test_failed_upgrade_does_not_block_app_startup();
    test_successful_upgrade_recovery_clears_failure_step();
    test_successful_upgrade_recovery_clears_staged_candidate();
    test_command_registration_and_help();
    test_status_command_writes_complete_line_atomically();
    test_memory_command_preserves_stats_identity_alias();
    test_mfg_gate_keeps_canonical_loader_in_command_mode();
    test_durable_install_request_precedes_fresh_mfg_gate();
    test_forced_command_mode_confirms_active_image();
    test_reboot_h2loader_commits_and_acknowledges_before_teardown();
    test_loader_reboot_command_bypasses_incomplete_mfg_gate();
    test_app_request_commits_once_before_ack_and_teardown();
    test_app_reboot_command_accepts_only_after_request_commit();
    test_boot_app_runs_disruptive_hook_before_reboot();
    test_upgrade_does_not_cancel_mfg_before_validation();
    test_last_result_is_persisted();
    test_wifi_connect_persists_confirmed_config();
    test_wifi_scan_is_bounded_and_structured();
    test_command_reinit_discards_partial_old_session_input();
    test_stage_close_removes_only_incomplete_tmp();
    test_url_stage_discards_old_candidate_before_wifi();
    test_stage_publication_preserves_app_lifecycle();
    test_stage_replacement_normalizes_only_legacy_staged_state();
    test_stage_replacement_cleans_files_after_preference_failure();
    test_startup_normalizes_legacy_staged_without_installing();
    test_stage_recovery_never_restores_discarded_candidate();
    test_app_client_coredump_uses_caller_output();
    test_app_return_console_prepares_before_success();
    test_app_return_console_reboots_when_reply_fails();
    test_app_client_requires_stable_hardware_capabilities();
    test_ble_identity();
    return 0;
}
