#include "h2_loader_command.h"
#include "h2_loader_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_LOADER_STAGE_CHUNK 1024u
#define H2_LOADER_STAGE_READ_TIMEOUT_MS 60000u
#define H2_LOADER_STAGE_TMP_PATH "/dl/update.tar.zlib.tmp"
#define H2_LOADER_STAGE_PATH "/dl/update.tar.zlib"
#define H2_LOADER_STAGE_PREV_PATH "/dl/update.tar.zlib.prev"
#define H2_LOADER_WIFI_SETTINGS_SETTLE_MS 250u
#define H2_LOADER_WIFI_READY_POLL_MS 250u
#define H2_LOADER_WIFI_READY_TIMEOUT_MS 30000u
#define H2_LOADER_WIFI_SCAN_DEFAULT_LIMIT 16u
#define H2_LOADER_WIFI_SCAN_MAX_LIMIT 16u
#define H2_LOADER_WIFI_SCAN_DEFAULT_TIMEOUT_MS 10000u
#define H2_LOADER_WIFI_SCAN_MAX_TIMEOUT_MS 30000u
#define H2_LOADER_HTTP_TIMEOUT_MS 600000
#define H2_LOADER_DOWNLOAD_CHUNK 4096u
#define H2_LOADER_DOWNLOAD_REPORT_STEP 65536u
#define H2_LOADER_COMMAND_PRINTF_BUFFER_SIZE 1024u
#define H2_LOADER_COMMAND_STATUS_BUFFER_SIZE 2048u

static int h2loader_get_coredump_partition(
    h2_loader_command_t *self,
    h2_pal_disk_partition_t *out_partition);
static int h2loader_get_coredump_info(
    h2_loader_command_t *self,
    const h2_pal_disk_partition_t *partition,
    uint64_t *out_stored_bytes,
    int *out_blank);

static uint32_t h2loader_effective_commands(
    h2_loader_command_t *self,
    const h2_loader_status_t *status) {
    uint32_t available = h2_loader_get_command_availability(
        self->config.loader, status);
    h2_pal_disk_partition_t partition;
    if (h2loader_get_coredump_partition(self, &partition) != H2_PAL_OK) {
        available &= ~(H2_LOADER_COMMAND_AVAILABLE_COREDUMP_STATUS |
            H2_LOADER_COMMAND_AVAILABLE_COREDUMP_DUMP |
            H2_LOADER_COMMAND_AVAILABLE_COREDUMP_ERASE);
    } else {
        uint64_t stored_bytes = 0u;
        int blank = 1;
        if (h2loader_get_coredump_info(
                self, &partition, &stored_bytes, &blank) != H2_PAL_OK) {
            available &= ~(H2_LOADER_COMMAND_AVAILABLE_COREDUMP_STATUS |
                H2_LOADER_COMMAND_AVAILABLE_COREDUMP_DUMP);
        } else if (blank || stored_bytes == 0u) {
            available &= ~H2_LOADER_COMMAND_AVAILABLE_COREDUMP_DUMP;
        }
    }
    return available;
}

static uint32_t h2loader_command_bit(
    size_t argc,
    const char *const *argv) {
    if (argc < 2u) return H2_LOADER_COMMAND_AVAILABLE_HELP;
    if (strcmp(argv[1], "help") == 0) return H2_LOADER_COMMAND_AVAILABLE_HELP;
    if (strcmp(argv[1], "status") == 0) return H2_LOADER_COMMAND_AVAILABLE_STATUS;
    if (strcmp(argv[1], "stats") == 0) return H2_LOADER_COMMAND_AVAILABLE_STATS;
    if (strcmp(argv[1], "memory") == 0) return H2_LOADER_COMMAND_AVAILABLE_MEMORY;
    if (strcmp(argv[1], "upgrade") == 0) return H2_LOADER_COMMAND_AVAILABLE_LOADER_UPGRADE;
    if (strcmp(argv[1], "reboot") == 0) {
        return argc >= 3u && strcmp(argv[2], "loader") == 0
            ? H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER
            : H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP;
    }
    if (strcmp(argv[1], "hold") == 0) {
        return argc >= 3u && strcmp(argv[2], "off") == 0
            ? H2_LOADER_COMMAND_AVAILABLE_HOLD_OFF
            : H2_LOADER_COMMAND_AVAILABLE_HOLD_ON;
    }
    if (strcmp(argv[1], "stage") == 0) {
        if (argc >= 3u && strcmp(argv[2], "abort") == 0)
            return H2_LOADER_COMMAND_AVAILABLE_STAGE_ABORT;
        if (argc >= 3u && strcmp(argv[2], "url") == 0)
            return H2_LOADER_COMMAND_AVAILABLE_STAGE_URL;
        return H2_LOADER_COMMAND_AVAILABLE_STAGE_PAYLOAD;
    }
    if (strcmp(argv[1], "wifi") == 0) {
        if (argc >= 3u && strcmp(argv[2], "scan") == 0)
            return H2_LOADER_COMMAND_AVAILABLE_WIFI_SCAN;
        if (argc >= 3u && strcmp(argv[2], "disconnect") == 0)
            return H2_LOADER_COMMAND_AVAILABLE_WIFI_DISCONNECT;
        return H2_LOADER_COMMAND_AVAILABLE_WIFI_CONNECT;
    }
    if (strcmp(argv[1], "coredump") == 0) {
        if (argc >= 3u && strcmp(argv[2], "dump") == 0)
            return H2_LOADER_COMMAND_AVAILABLE_COREDUMP_DUMP;
        if (argc >= 3u && strcmp(argv[2], "erase") == 0)
            return H2_LOADER_COMMAND_AVAILABLE_COREDUMP_ERASE;
        return H2_LOADER_COMMAND_AVAILABLE_COREDUMP_STATUS;
    }
    return 0u;
}

static h2_pal_result_t h2loader_require_command(
    h2_loader_command_t *self,
    size_t argc,
    const char *const *argv,
    h2_loader_status_t *out_status) {
    h2_loader_status_t status;
    const uint32_t bit = h2loader_command_bit(argc, argv);
    int rc;
    if (bit == H2_LOADER_COMMAND_AVAILABLE_HELP ||
        bit == H2_LOADER_COMMAND_AVAILABLE_MEMORY ||
        bit == H2_LOADER_COMMAND_AVAILABLE_WIFI_SCAN ||
        bit == H2_LOADER_COMMAND_AVAILABLE_WIFI_CONNECT ||
        bit == H2_LOADER_COMMAND_AVAILABLE_WIFI_DISCONNECT ||
        bit == H2_LOADER_COMMAND_AVAILABLE_COREDUMP_STATUS ||
        bit == H2_LOADER_COMMAND_AVAILABLE_COREDUMP_DUMP ||
        bit == H2_LOADER_COMMAND_AVAILABLE_COREDUMP_ERASE) {
        status = self->config.loader->status;
        status.command_availability = h2loader_effective_commands(self, &status);
    } else {
        rc = h2_loader_read_status(self->config.loader, &status);
        if (rc != H2_PAL_OK) return (h2_pal_result_t)rc;
        status.command_availability = h2loader_effective_commands(self, &status);
    }
    if (out_status != NULL) *out_status = status;
    return bit != 0u && (status.command_availability & bit) != 0u
        ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
static int h2loader_printf(h2_loader_command_t *self, const char *format, ...) {
    char line[H2_LOADER_COMMAND_PRINTF_BUFFER_SIZE];
    va_list args;
    int len;

    va_start(args, format);
    len = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (len < 0 || (size_t)len >= sizeof(line)) {
        return -1;
    }
    return h2_command_write(&self->command, line, (size_t)len) == H2_PAL_OK ? len : -1;
}

static h2_pal_result_t h2loader_write_line(
    h2_loader_command_t *self,
    const char *line) {
    char output[H2_LOADER_COMMAND_STATUS_BUFFER_SIZE + 1u];
    size_t len = strlen(line);
    if (len >= H2_LOADER_COMMAND_STATUS_BUFFER_SIZE) {
        return H2_PAL_ERR_NO_SPACE;
    }
    memcpy(output, line, len);
    output[len++] = '\n';
    return h2_command_write(&self->command, output, len);
}

static void h2loader_flush(FILE *stream) {
    (void)stream;
}

#define printf(...) h2loader_printf(self, __VA_ARGS__)
#define fflush h2loader_flush

static int h2loader_read_exact(
    h2_loader_command_t *self,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    return h2_command_read_exact(&self->command, data, len, timeout_ms);
}

static void h2loader_digest_abort(h2_loader_command_t *self) {
    if (self->config.digest.abort != NULL) {
        self->config.digest.abort(self->config.digest.user);
    }
}

static int h2loader_write_all(
    h2_loader_command_t *self,
    h2_pal_fs_file_t *file,
    const void *data,
    size_t len) {
    const uint8_t *cursor = (const uint8_t *)data;
    size_t remaining = len;

    while (remaining > 0u) {
        size_t written = 0u;
        int rc = h2_pal_fs_write(self->config.fs, file, cursor, remaining, &written);
        if (rc != H2_PAL_FS_OK) {
            return rc;
        }
        if (written == 0u || written > remaining) {
            return H2_PAL_ERR_IO;
        }
        cursor += written;
        remaining -= written;
    }
    return H2_PAL_OK;
}

static void h2loader_digest_hex(const uint8_t digest[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < 32u; ++i) {
        out[i * 2u] = hex[(digest[i] >> 4u) & 0x0fu];
        out[(i * 2u) + 1u] = hex[digest[i] & 0x0fu];
    }
    out[64] = '\0';
}

static void h2loader_stage_error(h2_loader_command_t *self, const char *step, int code) {
    printf("H2_LOADER_STAGE_ERROR step=%s code=%d\n", step, code);
    fflush(stdout);
}

static void h2loader_close_remove_tmp(h2_loader_command_t *self, h2_pal_fs_file_t *file) {
    if (file != NULL) {
        (void)h2_pal_fs_close(self->config.fs, file);
    }
    (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_TMP_PATH);
}

static int h2loader_publish_tmp_stage(h2_loader_command_t *self) {
    int rc = h2_loader_package_validate_path(
        &self->config.loader->package,
        H2_LOADER_STAGE_TMP_PATH);
    if (rc != H2_PAL_OK) {
        h2loader_stage_error(self, "validate", rc);
        (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_TMP_PATH);
        return rc;
    }
    rc = h2_loader_prepare_stage_publish(self->config.loader);
    if (rc != H2_PAL_OK) {
        h2loader_stage_error(self, "prepare_publish", rc);
        (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_TMP_PATH);
        return rc;
    }
    (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_PREV_PATH);
    rc = h2_pal_fs_rename(self->config.fs, H2_LOADER_STAGE_TMP_PATH, H2_LOADER_STAGE_PATH);
    if (rc != H2_PAL_FS_OK) {
        h2loader_stage_error(self, "rename_stage", rc);
        (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_TMP_PATH);
    }
    return rc;
}

static void h2loader_finish_stage_publish(h2_loader_command_t *self, int stage_rc) {
    if (stage_rc == H2_PAL_OK) {
        (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_PREV_PATH);
        return;
    }
    (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_PATH);
    (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_TMP_PATH);
    (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_PREV_PATH);
}

static int h2loader_receive_stage(
    h2_loader_command_t *self,
    size_t bytes,
    const char *expected_sha256) {
    uint8_t buffer[H2_LOADER_STAGE_CHUNK];
    uint8_t digest[32];
    char actual_sha256[65];
    h2_pal_fs_file_t *file = NULL;
    size_t remaining = bytes;
    int rc;

    if (expected_sha256 == NULL || strlen(expected_sha256) != 64u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_pal_fs_open(self->config.fs, H2_LOADER_STAGE_TMP_PATH, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    if (self->config.digest.start(self->config.digest.user) != H2_PAL_OK) {
        h2loader_close_remove_tmp(self, file);
        return H2_PAL_ERR_IO;
    }
    while (remaining > 0u) {
        size_t take = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        /* Reliable transports may spend several seconds recovering a lost
         * physical frame. Keep the stage reader alive for the same delivery
         * budget as the host, with enough margin for the host to stop first;
         * a replacement session still closes this read immediately through
         * the transport. */
        rc = h2loader_read_exact(
            self, buffer, take, H2_LOADER_STAGE_READ_TIMEOUT_MS);
        if (rc != H2_PAL_OK) {
            h2loader_digest_abort(self);
            h2loader_close_remove_tmp(self, file);
            return rc;
        }
        rc = self->config.digest.update(self->config.digest.user, buffer, take);
        if (rc != H2_PAL_OK) {
            h2loader_digest_abort(self);
            h2loader_close_remove_tmp(self, file);
            return rc;
        }
        rc = h2loader_write_all(self, file, buffer, take);
        if (rc != H2_PAL_FS_OK) {
            h2loader_digest_abort(self);
            h2loader_close_remove_tmp(self, file);
            return rc;
        }
        remaining -= take;
    }
    if (self->config.digest.finish(self->config.digest.user, digest) != H2_PAL_OK) {
        h2loader_digest_abort(self);
        h2loader_close_remove_tmp(self, file);
        return H2_PAL_ERR_IO;
    }
    h2loader_digest_abort(self);
    rc = h2_pal_fs_sync(self->config.fs, file);
    if (rc != H2_PAL_FS_OK) {
        h2loader_close_remove_tmp(self, file);
        return rc;
    }
    rc = h2_pal_fs_close(self->config.fs, file);
    file = NULL;
    if (rc != H2_PAL_FS_OK) {
        (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_TMP_PATH);
        return rc;
    }
    h2loader_digest_hex(digest, actual_sha256);
    if (strcmp(actual_sha256, expected_sha256) != 0) {
        printf("H2_LOADER_STAGE_ERROR step=checksum code=%d expected=%s actual=%s\n",
            H2_PAL_ERR_FORMAT,
            expected_sha256,
            actual_sha256);
        fflush(stdout);
        (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_TMP_PATH);
        return H2_PAL_ERR_FORMAT;
    }
    return h2loader_publish_tmp_stage(self);
}

typedef struct h2loader_download_context {
    h2_loader_command_t *command;
    h2_pal_fs_file_t *file;
    size_t expected_bytes;
    size_t written_bytes;
    size_t next_report_bytes;
} h2loader_download_context_t;

static int h2loader_download_read_cb(
    void *user,
    const h2_pal_http_request_t *request,
    const uint8_t *chunk,
    size_t chunk_len,
    size_t total_read,
    size_t remaining) {
    (void)request;
    (void)remaining;
    h2loader_download_context_t *ctx = (h2loader_download_context_t *)user;
    h2_loader_command_t *self = ctx != NULL ? ctx->command : NULL;
    if (ctx == NULL || ctx->file == NULL || chunk == NULL || self == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (total_read > ctx->expected_bytes || ctx->written_bytes + chunk_len > ctx->expected_bytes) {
        return H2_PAL_ERR_NO_SPACE;
    }
    int rc = self->config.digest.update(self->config.digest.user, chunk, chunk_len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2loader_write_all(self, ctx->file, chunk, chunk_len);
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    ctx->written_bytes += chunk_len;
    if (ctx->written_bytes >= ctx->next_report_bytes || ctx->written_bytes == ctx->expected_bytes) {
        printf("H2_LOADER_DOWNLOAD state=downloading bytes=%llu total=%llu\n",
            (unsigned long long)ctx->written_bytes,
            (unsigned long long)ctx->expected_bytes);
        fflush(stdout);
        ctx->next_report_bytes = ctx->written_bytes + H2_LOADER_DOWNLOAD_REPORT_STEP;
    }
    return H2_PAL_OK;
}

static int h2loader_stage_url(
    h2_loader_command_t *self,
    const char *url,
    size_t bytes,
    const char *expected_sha256) {
    uint8_t digest[32];
    char actual_sha256[65];
    h2_pal_fs_file_t *file = NULL;
    uint8_t chunk_buf[H2_LOADER_DOWNLOAD_CHUNK];
    h2_pal_http_response_t response;
    h2_pal_wifi_sta_status_t wifi_status;
    int rc;

    if (url == NULL || url[0] == '\0' || expected_sha256 == NULL || strlen(expected_sha256) != 64u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    printf("H2_LOADER_DOWNLOAD state=prepare step=wifi\n");
    fflush(stdout);
    for (uint32_t waited_ms = 0u;; waited_ms += H2_LOADER_WIFI_READY_POLL_MS) {
        rc = h2_pal_wifi_sta_get_status(self->config.wifi, &wifi_status);
        if (rc != H2_PAL_OK) {
            printf("H2_LOADER_DOWNLOAD state=error code=%d step=wifi\n", rc);
            fflush(stdout);
            return rc;
        }
        if (wifi_status.state == H2_PAL_WIFI_STA_STATE_GOT_IP &&
            wifi_status.ip_valid != 0u) {
            break;
        }
        if (waited_ms >= H2_LOADER_WIFI_READY_TIMEOUT_MS ||
            (wifi_status.state != H2_PAL_WIFI_STA_STATE_CONNECTING &&
             wifi_status.state != H2_PAL_WIFI_STA_STATE_CONNECTED &&
             wifi_status.state != H2_PAL_WIFI_STA_STATE_DISCONNECTED)) {
            printf("H2_LOADER_DOWNLOAD state=error code=%d step=wifi state=%d disconnect_reason=%d ip_valid=%u\n",
                H2_PAL_ERR_UNAVAILABLE,
                (int)wifi_status.state,
                wifi_status.disconnect_reason,
                (unsigned)wifi_status.ip_valid);
            fflush(stdout);
            return H2_PAL_ERR_UNAVAILABLE;
        }
        self->config.sleep_ms(
            self->config.clock_user, H2_LOADER_WIFI_READY_POLL_MS);
    }
    printf("H2_LOADER_DOWNLOAD state=prepare step=open path=%s\n", H2_LOADER_STAGE_TMP_PATH);
    fflush(stdout);
    rc = h2_pal_fs_open(self->config.fs, H2_LOADER_STAGE_TMP_PATH, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
    if (rc != H2_PAL_FS_OK) {
        printf("H2_LOADER_DOWNLOAD state=error code=%d step=open\n", rc);
        fflush(stdout);
        return rc;
    }
    printf("H2_LOADER_DOWNLOAD state=prepare step=sha_init\n");
    fflush(stdout);
    if (self->config.digest.start(self->config.digest.user) != H2_PAL_OK) {
        h2loader_close_remove_tmp(self, file);
        printf("H2_LOADER_DOWNLOAD state=error code=%d step=sha_init\n", H2_PAL_ERR_IO);
        fflush(stdout);
        return H2_PAL_ERR_IO;
    }

    h2loader_download_context_t context = {
        .command = self,
        .file = file,
        .expected_bytes = bytes,
        .written_bytes = 0u,
        .next_report_bytes = H2_LOADER_DOWNLOAD_REPORT_STEP,
    };
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = url, .len = strlen(url)},
        .timeout_ms = H2_LOADER_HTTP_TIMEOUT_MS,
        .retry_count = 0,
        .chunk_buf = chunk_buf,
        .chunk_buf_cap = sizeof(chunk_buf),
        .read_cb = h2loader_download_read_cb,
        .user = &context,
        .allocator = self->config.loader->config.package.allocator,
    };
    h2_pal_http_response_reset(&response);
    printf("H2_LOADER_DOWNLOAD state=connecting url=%s\n", url);
    fflush(stdout);
    rc = h2_pal_http_request(self->config.http, &request, &response);
    if (rc != H2_PAL_OK) {
        h2loader_digest_abort(self);
        h2loader_close_remove_tmp(self, file);
        printf("H2_LOADER_DOWNLOAD state=%s code=%d step=http\n", rc == H2_PAL_ERR_TIMEOUT ? "timeout" : "error", rc);
        fflush(stdout);
        return rc;
    }
    if (h2_pal_http_status_has_error(response.status_code)) {
        h2loader_digest_abort(self);
        h2loader_close_remove_tmp(self, file);
        printf("H2_LOADER_DOWNLOAD state=error code=%d step=http_status status=%d\n",
            H2_PAL_ERR_IO,
            response.status_code);
        fflush(stdout);
        return H2_PAL_ERR_IO;
    }
    if (context.written_bytes != bytes) {
        h2loader_digest_abort(self);
        h2loader_close_remove_tmp(self, file);
        printf("H2_LOADER_DOWNLOAD state=error code=%d step=size bytes=%llu total=%llu\n",
            H2_PAL_ERR_FORMAT,
            (unsigned long long)context.written_bytes,
            (unsigned long long)bytes);
        fflush(stdout);
        return H2_PAL_ERR_FORMAT;
    }
    if (self->config.digest.finish(self->config.digest.user, digest) != H2_PAL_OK) {
        h2loader_digest_abort(self);
        h2loader_close_remove_tmp(self, file);
        printf("H2_LOADER_DOWNLOAD state=error code=%d step=sha_finish\n", H2_PAL_ERR_IO);
        fflush(stdout);
        return H2_PAL_ERR_IO;
    }
    h2loader_digest_abort(self);
    rc = h2_pal_fs_sync(self->config.fs, file);
    if (rc != H2_PAL_FS_OK) {
        h2loader_close_remove_tmp(self, file);
        printf("H2_LOADER_DOWNLOAD state=error code=%d step=sync\n", rc);
        fflush(stdout);
        return rc;
    }
    rc = h2_pal_fs_close(self->config.fs, file);
    file = NULL;
    if (rc != H2_PAL_FS_OK) {
        (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_TMP_PATH);
        printf("H2_LOADER_DOWNLOAD state=error code=%d step=close\n", rc);
        fflush(stdout);
        return rc;
    }
    h2loader_digest_hex(digest, actual_sha256);
    if (strcmp(actual_sha256, expected_sha256) != 0) {
        (void)h2_pal_fs_remove(self->config.fs, H2_LOADER_STAGE_TMP_PATH);
        printf("H2_LOADER_DOWNLOAD state=checksum_fail expected=%s actual=%s\n", expected_sha256, actual_sha256);
        fflush(stdout);
        return H2_PAL_ERR_FORMAT;
    }
    rc = h2loader_publish_tmp_stage(self);
    if (rc != H2_PAL_OK) {
        printf("H2_LOADER_DOWNLOAD state=error code=%d step=publish\n", rc);
        fflush(stdout);
        return rc;
    }
    printf("H2_LOADER_DOWNLOAD state=done bytes=%llu sha256=%s\n",
        (unsigned long long)bytes,
        actual_sha256);
    fflush(stdout);
    return H2_PAL_OK;
}

static int h2loader_parse_size_arg(const char *text, size_t *out_value) {
    char *end = NULL;
    unsigned long long value;
    if (text == NULL || out_value == NULL || text[0] == '\0') {
        return H2_PAL_ERR_INVALID_ARG;
    }
    value = strtoull(text, &end, 10);
    if (end == NULL || *end != '\0' || value > UINT32_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = (size_t)value;
    return H2_PAL_OK;
}

static int h2loader_copy_wifi_text(char *dst, size_t dst_cap, size_t *out_len, const char *src) {
    size_t len;
    if (dst == NULL || out_len == NULL || src == NULL || dst_cap == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    len = strlen(src);
    if (len + 1u > dst_cap) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(dst, src, len + 1u);
    *out_len = len;
    return H2_PAL_OK;
}

typedef struct h2loader_wifi_scan_context {
    h2_loader_command_t *command;
    size_t limit;
    size_t count;
    int output_result;
} h2loader_wifi_scan_context_t;

static bool h2loader_wifi_scan_result(
    void *user,
    const h2_pal_wifi_scan_entry_t *entry) {
    static const char hex[] = "0123456789abcdef";
    h2loader_wifi_scan_context_t *context = user;
    h2_loader_command_t *self;
    char ssid_hex[(H2_PAL_WIFI_SSID_MAX * 2u) + 1u];

    if (context == NULL) {
        return false;
    }
    if (entry == NULL || entry->ssid_len > H2_PAL_WIFI_SSID_MAX) {
        context->output_result = H2_PAL_ERR_FORMAT;
        return false;
    }
    if (context->count >= context->limit ||
        context->output_result != H2_PAL_OK) {
        return false;
    }
    self = context->command;
    for (size_t i = 0u; i < entry->ssid_len; ++i) {
        uint8_t byte = (uint8_t)entry->ssid[i];
        ssid_hex[i * 2u] = hex[(byte >> 4u) & 0x0fu];
        ssid_hex[(i * 2u) + 1u] = hex[byte & 0x0fu];
    }
    ssid_hex[entry->ssid_len * 2u] = '\0';
    context->count++;
    if (printf(
            "H2_LOADER_WIFI_SCAN_RESULT index=%u ssid_hex=%s "
            "bssid=%02x%02x%02x%02x%02x%02x channel=%u rssi=%d security=%d\n",
            (unsigned)context->count,
            ssid_hex,
            entry->bssid[0], entry->bssid[1], entry->bssid[2],
            entry->bssid[3], entry->bssid[4], entry->bssid[5],
            (unsigned)entry->channel,
            entry->rssi,
            (int)entry->security) < 0) {
        context->output_result = H2_PAL_ERR_IO;
        return false;
    }
    fflush(stdout);
    return context->count < context->limit;
}

static int h2loader_wifi_scan_command(
    h2_loader_command_t *self,
    size_t argc,
    const char *const *argv) {
    h2loader_wifi_scan_context_t context = {
        .command = self,
        .limit = H2_LOADER_WIFI_SCAN_DEFAULT_LIMIT,
        .output_result = H2_PAL_OK,
    };
    size_t timeout_ms = H2_LOADER_WIFI_SCAN_DEFAULT_TIMEOUT_MS;
    int saw_limit = 0;
    int saw_timeout = 0;

    for (size_t i = 3u; i < argc; i += 2u) {
        size_t value = 0u;
        if (i + 1u >= argc ||
            h2loader_parse_size_arg(argv[i + 1u], &value) != H2_PAL_OK) {
            printf("usage: h2loader wifi scan [--limit <1-16>] [--timeout-ms <1-30000>]\n");
            return H2_PAL_ERR_INVALID_ARG;
        }
        if (strcmp(argv[i], "--limit") == 0 && !saw_limit &&
            value > 0u && value <= H2_LOADER_WIFI_SCAN_MAX_LIMIT) {
            context.limit = value;
            saw_limit = 1;
        } else if (strcmp(argv[i], "--timeout-ms") == 0 && !saw_timeout &&
            value > 0u && value <= H2_LOADER_WIFI_SCAN_MAX_TIMEOUT_MS) {
            timeout_ms = value;
            saw_timeout = 1;
        } else {
            printf("usage: h2loader wifi scan [--limit <1-16>] [--timeout-ms <1-30000>]\n");
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    int rc = h2_pal_wifi_sta_scan(
        self->config.wifi,
        NULL,
        h2loader_wifi_scan_result,
        &context,
        (uint32_t)timeout_ms);
    if (context.output_result != H2_PAL_OK) {
        rc = context.output_result;
    }
    printf("H2_LOADER_WIFI_SCAN_DONE result=%s code=%d count=%u\n",
        rc == H2_PAL_OK ? "OK" : "error",
        rc,
        (unsigned)context.count);
    fflush(stdout);
    return rc;
}

static int h2loader_wifi_command(
    h2_loader_command_t *self,
    size_t argc,
    const char *const *argv) {
    const h2_pal_wifi_sta_api_t *sta = self->config.wifi;
    int rc;
    if (argc >= 3 && strcmp(argv[2], "scan") == 0) {
        return h2loader_wifi_scan_command(self, argc, argv);
    }
    if (argc >= 3 && strcmp(argv[2], "connect") == 0) {
        h2_pal_wifi_sta_config_t config;
        h2_pal_wifi_sta_status_t status;
        if (argc < 5) {
            printf("usage: h2loader wifi connect <ssid> <password>\n");
            return H2_PAL_ERR_INVALID_ARG;
        }
        memset(&config, 0, sizeof(config));
        rc = h2loader_copy_wifi_text(config.ssid, sizeof(config.ssid), &config.ssid_len, argv[3]);
        if (rc == H2_PAL_OK) {
            rc = h2loader_copy_wifi_text(config.password, sizeof(config.password), &config.password_len, argv[4]);
        }
        if (rc != H2_PAL_OK) {
            printf("H2_LOADER_WIFI result=invalid_config code=%d\n", rc);
            return rc;
        }
        if (self->config.wifi_settings != NULL) {
            rc = h2_pal_wifi_settings_set_saved_sta_config(
                self->config.wifi_settings,
                &config);
            if (rc != H2_PAL_OK) {
                printf(
                    "H2_LOADER_WIFI result=error code=%d step=settings\n",
                    rc);
                return rc;
            }
            self->config.sleep_ms(
                self->config.clock_user,
                H2_LOADER_WIFI_SETTINGS_SETTLE_MS);
        }
        rc = h2_pal_wifi_sta_connect(sta, &config, 0u);
        if (rc != H2_PAL_OK) {
            memset(&status, 0, sizeof(status));
            if (h2_pal_wifi_sta_get_status(sta, &status) == H2_PAL_OK) {
                printf("H2_LOADER_WIFI result=error code=%d state=%d disconnect_reason=%d ip_valid=%u\n",
                    rc,
                    (int)status.state,
                    status.disconnect_reason,
                    (unsigned)status.ip_valid);
            } else {
                printf("H2_LOADER_WIFI result=error code=%d\n", rc);
            }
            return rc;
        }
        printf("H2_LOADER_WIFI result=connecting ssid=%s\n", config.ssid);
        fflush(stdout);
        return H2_PAL_OK;
    }
    if (argc >= 3 && strcmp(argv[2], "disconnect") == 0) {
        rc = h2_pal_wifi_sta_disconnect(sta);
        printf("H2_LOADER_WIFI result=%s code=%d\n",
            rc == H2_PAL_OK ? "disconnected" : "error", rc);
        fflush(stdout);
        return rc;
    }
    printf("usage: h2loader wifi <scan|connect|disconnect>\n");
    return H2_PAL_ERR_INVALID_ARG;
}

static int h2loader_get_coredump_partition(
    h2_loader_command_t *self,
    h2_pal_disk_partition_t *out_partition) {
    const h2_pal_disk_api_t *disk = self->config.disk;

    if (disk == NULL || out_partition == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return h2_pal_disk_get_partition(disk, self->config.coredump_partition_id, out_partition);
}

static int h2loader_get_coredump_info(
    h2_loader_command_t *self,
    const h2_pal_disk_partition_t *partition,
    uint64_t *out_stored_bytes,
    int *out_blank
) {
    const h2_pal_disk_api_t *disk = self->config.disk;
    uint8_t head[16];
    uint32_t size;
    int rc;

    if (partition == NULL || out_stored_bytes == NULL || out_blank == NULL ||
        disk == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    *out_stored_bytes = 0u;
    *out_blank = 1;
    rc = h2_pal_disk_read(disk, partition->id, 0u, head, sizeof(head));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < sizeof(head); ++i) {
        if (head[i] != 0xffu && head[i] != 0x00u) {
            *out_blank = 0;
            break;
        }
    }
    if (*out_blank) {
        return H2_PAL_OK;
    }
    size = (uint32_t)head[0] |
        ((uint32_t)head[1] << 8u) |
        ((uint32_t)head[2] << 16u) |
        ((uint32_t)head[3] << 24u);
    if (size < sizeof(uint32_t) || size > partition->size) {
        return H2_PAL_ERR_FORMAT;
    }
    *out_stored_bytes = size;
    return H2_PAL_OK;
}

static int h2loader_coredump_command(
    h2_loader_command_t *self,
    size_t argc,
    const char *const *argv) {
    h2_pal_disk_partition_t partition;
    const h2_pal_disk_api_t *disk = self->config.disk;
    const char *sub = argc >= 3 ? argv[2] : "status";
    int rc = h2loader_get_coredump_partition(self, &partition);

    if (rc != H2_PAL_OK) {
        printf("H2_LOADER_COREDUMP command=%s result=unavailable code=%d\n", sub, rc);
        return rc;
    }
    if (strcmp(sub, "status") == 0) {
        uint64_t stored_bytes = 0u;
        int blank = 1;
        rc = h2loader_get_coredump_info(self, &partition, &stored_bytes, &blank);
        printf("H2_LOADER_COREDUMP_STATUS result=%s code=%d partition=%s bytes=%llu stored_bytes=%llu blank=%d\n",
            rc == H2_PAL_OK ? "OK" : "fail",
            rc,
            partition.name,
            (unsigned long long)partition.size,
            (unsigned long long)stored_bytes,
            blank);
        return rc;
    }
    if (strcmp(sub, "erase") == 0) {
        rc = h2_pal_disk_erase(disk, partition.id, 0u, partition.size);
        printf("H2_LOADER_COREDUMP_ERASE result=%s code=%d\n", rc == H2_PAL_OK ? "OK" : "fail", rc);
        return rc;
    }
    if (strcmp(sub, "dump") == 0) {
        static const char hex[] = "0123456789abcdef";
        uint8_t buffer[128];
        char data_line[384];
        uint64_t stored_bytes = 0u;
        uint64_t offset = 0u;
        int blank = 1;
        rc = h2loader_get_coredump_info(self, &partition, &stored_bytes, &blank);
        if (rc != H2_PAL_OK) {
            printf("H2_LOADER_COREDUMP_DUMP result=fail code=%d offset=0\n", rc);
            return rc;
        }
        while (offset < stored_bytes) {
            size_t take = stored_bytes - offset > sizeof(buffer) ? sizeof(buffer) : (size_t)(stored_bytes - offset);
            rc = h2_pal_disk_read(disk, partition.id, offset, buffer, take);
            if (rc != H2_PAL_OK) {
                printf("H2_LOADER_COREDUMP_DUMP result=fail code=%d offset=%llu\n", rc, (unsigned long long)offset);
                return rc;
            }
            int prefix_len = snprintf(
                data_line,
                sizeof(data_line),
                "H2_LOADER_COREDUMP_DATA offset=%llu hex=",
                (unsigned long long)offset);
            if (prefix_len < 0 ||
                (size_t)prefix_len + (take * 2u) + 2u > sizeof(data_line)) {
                return H2_PAL_ERR_NO_SPACE;
            }
            for (size_t i = 0u; i < take; ++i) {
                data_line[(size_t)prefix_len + (i * 2u)] =
                    hex[(buffer[i] >> 4u) & 0x0fu];
                data_line[(size_t)prefix_len + (i * 2u) + 1u] =
                    hex[buffer[i] & 0x0fu];
            }
            data_line[(size_t)prefix_len + (take * 2u)] = '\n';
            data_line[(size_t)prefix_len + (take * 2u) + 1u] = '\0';
            printf("%s", data_line);
            offset += take;
        }
        printf("H2_LOADER_COREDUMP_DUMP result=OK bytes=%llu blank=%d\n",
            (unsigned long long)stored_bytes,
            blank);
        return H2_PAL_OK;
    }
    printf("H2_LOADER_COREDUMP result=invalid_command command=%s\n", sub);
    return H2_PAL_ERR_INVALID_ARG;
}

static h2_pal_result_t h2loader_root_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    h2_loader_command_t *self = (h2_loader_command_t *)user;

    (void)command;
    h2_pal_result_t availability = h2loader_require_command(
        self, argc, argv, NULL);
    if (availability != H2_PAL_OK) return availability;
    if (argc < 2u) {
        printf("usage: h2loader <help|status|stats|memory|wifi|stage|upgrade|reboot [app|loader]|hold|coredump>\n");
    } else {
        printf("usage: h2loader <help|status|memory|stage|upgrade|reboot [app|loader]|hold|coredump>\n");
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2loader_help_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    h2_loader_command_t *self = (h2_loader_command_t *)user;

    (void)command;
    h2_pal_result_t availability = h2loader_require_command(
        self, argc, argv, NULL);
    if (availability != H2_PAL_OK) return availability;
    printf("h2loader <help|status|stats|memory|wifi|stage|upgrade|reboot [app|loader]|hold|coredump>\n");
    return H2_PAL_OK;
}

static h2_pal_result_t h2loader_status_handler_unlocked(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    h2_loader_command_t *self = (h2_loader_command_t *)user;
    h2_loader_status_t status;
    char line[H2_LOADER_COMMAND_STATUS_BUFFER_SIZE];
    int rc;

    (void)command;
    (void)argc;
    (void)argv;
    rc = h2loader_require_command(self, argc, argv, &status);
    if (rc != H2_PAL_OK) {
        printf("H2_LOADER_STATUS_ERROR code=%d\n", rc);
        return (h2_pal_result_t)rc;
    }
    rc = h2_loader_status_format(&status, line, sizeof(line));
    if (rc != H2_PAL_OK) {
        return (h2_pal_result_t)rc;
    }
    return h2loader_write_line(self, line);
}

static h2_pal_result_t h2loader_memory_handler_unlocked(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    h2_loader_command_t *self = (h2_loader_command_t *)user;
    h2_loader_memory_stats_t stats = {0};
    h2_pal_result_t rc;

    (void)command;
    (void)argc;
    (void)argv;
    if (self->config.memory_stats.read == NULL) {
        printf("H2_LOADER_MEMORY result=unsupported code=%d\n", H2_PAL_ERR_UNSUPPORTED);
        return H2_PAL_ERR_UNSUPPORTED;
    }
    rc = self->config.memory_stats.read(self->config.memory_stats.user, &stats);
    if (rc != H2_PAL_OK) {
        printf("H2_LOADER_MEMORY result=fail code=%d\n", rc);
        return rc;
    }
    printf(
        "H2_LOADER_MEMORY result=OK internal_total=%zu internal_free=%zu "
        "internal_min_free=%zu internal_largest=%zu iram_total=%zu iram_free=%zu "
        "iram_min_free=%zu iram_largest=%zu psram_total=%zu psram_free=%zu "
        "psram_min_free=%zu psram_largest=%zu\n",
        stats.internal.total_bytes, stats.internal.free_bytes,
        stats.internal.minimum_free_bytes, stats.internal.largest_free_block_bytes,
        stats.iram.total_bytes, stats.iram.free_bytes,
        stats.iram.minimum_free_bytes, stats.iram.largest_free_block_bytes,
        stats.psram.total_bytes, stats.psram.free_bytes,
        stats.psram.minimum_free_bytes, stats.psram.largest_free_block_bytes);
    return H2_PAL_OK;
}

static h2_pal_result_t h2loader_wifi_handler_unlocked(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    h2_loader_command_t *self = (h2_loader_command_t *)user;

    (void)command;
    return (h2_pal_result_t)h2loader_wifi_command(self, argc, argv);
}

static h2_pal_result_t h2loader_stage_handler_unlocked(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    h2_loader_command_t *self = (h2_loader_command_t *)user;
    size_t bytes = 0u;
    int rc;

    (void)command;
    if (argc >= 3u && strcmp(argv[2], "abort") == 0) {
        rc = h2_loader_abort_stage(self->config.loader);
        printf("H2_LOADER_STAGE_ABORT result=%s code=%d\n",
            rc == H2_PAL_OK ? "OK" : "fail",
            rc);
        return (h2_pal_result_t)rc;
    }
    if (argc >= 3u && strcmp(argv[2], "url") == 0) {
        if (argc < 6u) {
            printf("usage: h2loader stage url <url> <bytes> <sha256>\n");
            return H2_PAL_ERR_INVALID_ARG;
        }
        rc = h2loader_parse_size_arg(argv[4], &bytes);
        if (rc == H2_PAL_OK &&
            (argv[3] == NULL || argv[3][0] == '\0' ||
                argv[5] == NULL || strlen(argv[5]) != 64u)) {
            rc = H2_PAL_ERR_INVALID_ARG;
        }
        if (rc != H2_PAL_OK) {
            return (h2_pal_result_t)rc;
        }
        rc = h2_loader_begin_stage_replacement(
            self->config.loader,
            H2_LOADER_STAGE_TMP_PATH,
            H2_LOADER_STAGE_PREV_PATH);
        if (rc != H2_PAL_OK) {
            printf(
                "H2_LOADER_DOWNLOAD state=error code=%d step=replace\n",
                rc);
            return (h2_pal_result_t)rc;
        }
        rc = h2loader_stage_url(self, argv[3], bytes, argv[5]);
        if (rc != H2_PAL_OK) {
            return (h2_pal_result_t)rc;
        }
        rc = h2_loader_publish_stage(self->config.loader, (uint32_t)bytes, argv[5]);
        h2loader_finish_stage_publish(self, rc);
        printf("H2_LOADER_STAGE result=%s code=%d\n",
            rc == H2_PAL_OK ? "OK" : "fail",
            rc);
        return (h2_pal_result_t)rc;
    }
    if (argc < 4u) {
        printf("usage: h2loader stage <bytes> <sha256>\n");
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2loader_parse_size_arg(argv[2], &bytes);
    if (rc == H2_PAL_OK &&
        (argv[3] == NULL || strlen(argv[3]) != 64u)) {
        rc = H2_PAL_ERR_INVALID_ARG;
    }
    if (rc == H2_PAL_OK) {
        rc = h2_loader_begin_stage_replacement(
            self->config.loader,
            H2_LOADER_STAGE_TMP_PATH,
            H2_LOADER_STAGE_PREV_PATH);
    }
    if (rc == H2_PAL_OK) {
        rc = h2loader_receive_stage(self, bytes, argv[3]);
    }
    printf("H2_LOADER_STAGE_RECEIVE result=%s code=%d\n",
        rc == H2_PAL_OK ? "OK" : "fail",
        rc);
    if (rc != H2_PAL_OK) {
        return (h2_pal_result_t)rc;
    }
    rc = h2_loader_publish_stage(self->config.loader, (uint32_t)bytes, argv[3]);
    h2loader_finish_stage_publish(self, rc);
    printf("H2_LOADER_STAGE result=%s code=%d\n",
        rc == H2_PAL_OK ? "OK" : "fail",
        rc);
    return (h2_pal_result_t)rc;
}

typedef struct h2loader_upgrade_transition_context {
    h2_loader_command_t *self;
    int emitted;
} h2loader_upgrade_transition_context_t;

static int h2loader_upgrade_transition(void *user) {
    h2loader_upgrade_transition_context_t *context =
        (h2loader_upgrade_transition_context_t *)user;
    h2_loader_command_t *self = context->self;
    (void)printf(
        "H2_LOADER_UPGRADE result=OK code=0 transition=reboot\n");
    context->emitted = 1;
    /* Waiting for the transport ACK is best-effort because the host may close
     * the session as soon as it receives the accepted transition. */
    (void)h2_command_flush(&self->command);
    return H2_PAL_OK;
}

static h2_pal_result_t h2loader_upgrade_handler_unlocked(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    h2_loader_command_t *self = (h2_loader_command_t *)user;
    h2loader_upgrade_transition_context_t transition = {
        .self = self,
    };
    int rc;

    (void)command;
    (void)argc;
    (void)argv;
    rc = h2_loader_upgrade_start_with_transition(
        self->config.loader, h2loader_upgrade_transition, &transition);
    if (!transition.emitted || rc != H2_PAL_OK) {
        printf("H2_LOADER_UPGRADE result=%s code=%d transition=%s\n",
            rc == H2_PAL_OK ? "OK" : "fail",
            rc,
            rc == H2_PAL_OK ? "none" : "failed");
    }
    return (h2_pal_result_t)rc;
}

typedef struct h2loader_reboot_transition_context {
    h2_loader_command_t *self;
    const char *target;
    int emitted;
} h2loader_reboot_transition_context_t;

static int h2loader_reboot_transition(void *user) {
    h2loader_reboot_transition_context_t *context =
        (h2loader_reboot_transition_context_t *)user;
    h2_loader_command_t *self = context->self;

    printf(
        "H2_LOADER_REBOOT target=%s result=accepted\n",
        context->target);
    context->emitted = 1;
    /* Give the transport a bounded chance to publish the accepted transition
       before disruptive teardown. The lifecycle request is already durable,
       so a missing peer ACK must not cancel installation or the whole-device
       reset. */
    (void)h2_command_flush(&self->command);
    return H2_PAL_OK;
}

static void h2loader_reboot_failure(
    h2loader_reboot_transition_context_t *transition,
    int result) {
    h2_loader_command_t *self = transition->self;

    if (transition->emitted) {
        return;
    }
    printf(
        "H2_LOADER_REBOOT target=%s result=fail code=%d\n",
        transition->target,
        result);
    (void)h2_command_flush(&transition->self->command);
}

static h2_pal_result_t h2loader_reboot_handler_unlocked(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    h2_loader_command_t *self = (h2_loader_command_t *)user;
    h2loader_reboot_transition_context_t transition = {
        .self = self,
    };
    int rc;

    (void)command;
    if (argc >= 3u && strcmp(argv[2], "loader") == 0) {
        transition.target = "loader";
        rc = h2_loader_reboot_h2loader_with_transition(
            self->config.loader,
            h2loader_reboot_transition,
            &transition);
        h2loader_reboot_failure(&transition, rc);
        if (rc == H2_PAL_OK) {
            printf(
                "H2_LOADER_REBOOT_FINAL target=loader result=OK code=0\n");
        } else if (transition.emitted) {
            printf(
                "H2_LOADER_REBOOT_FINAL target=loader result=fail code=%d\n",
                rc);
        }
        return (h2_pal_result_t)rc;
    }
    if (argc >= 3u && strcmp(argv[2], "app") != 0) {
        printf("usage: h2loader reboot [app|loader]\n");
        return H2_PAL_ERR_INVALID_ARG;
    }
    transition.target = "app";
    rc = self->config.defer_app_install ?
        h2_loader_request_install_staged_with_transition(
            self->config.loader,
            h2loader_reboot_transition,
            &transition) :
        h2_loader_install_staged_with_transition(
            self->config.loader,
            h2loader_reboot_transition,
            &transition);
    h2loader_reboot_failure(&transition, rc);
    return (h2_pal_result_t)rc;
}

static h2_pal_result_t h2loader_hold_handler_unlocked(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    h2_loader_command_t *self = (h2_loader_command_t *)user;
    int rc;

    (void)command;
    if (argc < 3u || (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
        printf("usage: h2loader hold <on|off>\n");
        return H2_PAL_OK;
    }
    rc = h2_loader_set_hold(self->config.loader, strcmp(argv[2], "on") == 0);
    printf("H2_LOADER_HOLD result=%s code=%d\n",
        rc == H2_PAL_OK ? "OK" : "fail",
        rc);
    return (h2_pal_result_t)rc;
}

static h2_pal_result_t h2loader_coredump_handler_unlocked(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    h2_loader_command_t *self = (h2_loader_command_t *)user;

    (void)command;
    return (h2_pal_result_t)h2loader_coredump_command(self, argc, argv);
}

typedef h2_pal_result_t (*h2loader_handler_fn)(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv);

static h2_pal_result_t h2loader_invoke_locked(
    h2_loader_command_t *self,
    h2_command_t *command,
    size_t argc,
    const char *const *argv,
    int lock_wifi,
    int lock_operation,
    h2loader_handler_fn handler) {
    int wifi_locked = 0;
    int operation_locked = 0;
    h2_pal_result_t result;

    if (self == NULL || handler == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (lock_wifi && self->config.wifi_operation_mutex != NULL) {
        result = h2_pal_mutex_lock(
            self->config.wifi_operation_sync,
            self->config.wifi_operation_mutex);
        if (result != H2_PAL_OK) {
            return result;
        }
        wifi_locked = 1;
    }
    if (lock_operation && self->config.operation_mutex != NULL) {
        result = h2_pal_mutex_lock(
            self->config.operation_sync,
            self->config.operation_mutex);
        if (result != H2_PAL_OK) {
            if (wifi_locked) {
                (void)h2_pal_mutex_unlock(
                    self->config.wifi_operation_sync,
                    self->config.wifi_operation_mutex);
            }
            return result;
        }
        operation_locked = 1;
    }
    result = h2loader_require_command(self, argc, argv, NULL);
    if (result == H2_PAL_OK) {
        result = handler(self, command, argc, argv);
    }
    if (operation_locked) {
        int unlock_rc = h2_pal_mutex_unlock(
            self->config.operation_sync,
            self->config.operation_mutex);
        if (result == H2_PAL_OK) {
            result = (h2_pal_result_t)unlock_rc;
        }
    }
    if (wifi_locked) {
        int unlock_rc = h2_pal_mutex_unlock(
            self->config.wifi_operation_sync,
            self->config.wifi_operation_mutex);
        if (result == H2_PAL_OK) {
            result = (h2_pal_result_t)unlock_rc;
        }
    }
    return result;
}

static h2_pal_result_t h2loader_status_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    return h2loader_invoke_locked(
        (h2_loader_command_t *)user, command, argc, argv, 0, 1,
        h2loader_status_handler_unlocked);
}

static h2_pal_result_t h2loader_memory_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    return h2loader_invoke_locked(
        (h2_loader_command_t *)user, command, argc, argv, 0, 0,
        h2loader_memory_handler_unlocked);
}

static h2_pal_result_t h2loader_wifi_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    return h2loader_invoke_locked(
        (h2_loader_command_t *)user, command, argc, argv, 1, 0,
        h2loader_wifi_handler_unlocked);
}

static h2_pal_result_t h2loader_stage_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    int lock_wifi = argc >= 3u && strcmp(argv[2], "url") == 0;
    return h2loader_invoke_locked(
        (h2_loader_command_t *)user, command, argc, argv, lock_wifi, 1,
        h2loader_stage_handler_unlocked);
}

static h2_pal_result_t h2loader_upgrade_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    return h2loader_invoke_locked(
        (h2_loader_command_t *)user, command, argc, argv, 0, 1,
        h2loader_upgrade_handler_unlocked);
}

static h2_pal_result_t h2loader_reboot_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    return h2loader_invoke_locked(
        (h2_loader_command_t *)user, command, argc, argv, 0, 1,
        h2loader_reboot_handler_unlocked);
}

static h2_pal_result_t h2loader_hold_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    return h2loader_invoke_locked(
        (h2_loader_command_t *)user, command, argc, argv, 0, 1,
        h2loader_hold_handler_unlocked);
}

static h2_pal_result_t h2loader_coredump_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    return h2loader_invoke_locked(
        (h2_loader_command_t *)user, command, argc, argv, 0, 1,
        h2loader_coredump_handler_unlocked);
}

int h2_loader_command_init(
    h2_loader_command_t *self,
    const h2_loader_command_config_t *config) {
    static const struct {
        const char *path;
        h2_command_handler_fn handler;
    } registrations[] = {
        {"h2loader", h2loader_root_handler},
        {"h2loader help", h2loader_help_handler},
        {"h2loader status", h2loader_status_handler},
        {"h2loader stats", h2loader_status_handler},
        {"h2loader memory", h2loader_memory_handler},
        {"h2loader wifi", h2loader_wifi_handler},
        {"h2loader stage", h2loader_stage_handler},
        {"h2loader upgrade", h2loader_upgrade_handler},
        {"h2loader reboot", h2loader_reboot_handler},
        {"h2loader hold", h2loader_hold_handler},
        {"h2loader coredump", h2loader_coredump_handler},
    };
    h2_command_config_t command_config;
    int rc;

    if (self == NULL || config == NULL ||
        config->loader == NULL || config->fs == NULL ||
        config->http == NULL || config->wifi == NULL || config->disk == NULL ||
        config->digest.start == NULL || config->digest.update == NULL ||
        config->digest.finish == NULL || config->now_ms == NULL ||
        config->sleep_ms == NULL || config->io.vtable == NULL ||
        ((config->operation_sync == NULL) !=
            (config->operation_mutex == NULL)) ||
        ((config->wifi_operation_sync == NULL) !=
            (config->wifi_operation_mutex == NULL))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(self, 0, sizeof(*self));
    self->config = *config;
    memset(&command_config, 0, sizeof(command_config));
    command_config.io = config->io;
    command_config.definitions = self->definitions;
    command_config.definition_capacity = H2_LOADER_COMMAND_DEFINITION_CAPACITY;
    command_config.routes = self->routes;
    command_config.route_nodes = self->route_nodes;
    command_config.route_node_capacity = H2_LOADER_COMMAND_ROUTE_NODE_CAPACITY;
    command_config.input_buffer = self->input_buffer;
    command_config.input_buffer_size = sizeof(self->input_buffer);
    command_config.argv = self->argv;
    command_config.argv_capacity = H2_LOADER_COMMAND_ARGV_CAPACITY;
    command_config.write_timeout_ms = 5000u;
    rc = h2_command_init(&self->command, &command_config);
    for (size_t i = 0u;
         rc == H2_PAL_OK && i < sizeof(registrations) / sizeof(registrations[0]);
         ++i) {
        h2_command_definition_t definition = {
            .path = registrations[i].path,
            .help = NULL,
            .handler = registrations[i].handler,
            .user = self,
        };
        rc = h2_command_register(&self->command, &definition);
    }
    if (rc == H2_PAL_OK) {
        uint32_t implemented = H2_LOADER_COMMAND_AVAILABLE_HELP |
            H2_LOADER_COMMAND_AVAILABLE_STATUS |
            H2_LOADER_COMMAND_AVAILABLE_STATS |
            H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP |
            H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER |
            H2_LOADER_COMMAND_AVAILABLE_STAGE_PAYLOAD |
            H2_LOADER_COMMAND_AVAILABLE_STAGE_ABORT |
            H2_LOADER_COMMAND_AVAILABLE_STAGE_URL |
            H2_LOADER_COMMAND_AVAILABLE_HOLD_ON |
            H2_LOADER_COMMAND_AVAILABLE_HOLD_OFF |
            H2_LOADER_COMMAND_AVAILABLE_WIFI_SCAN |
            H2_LOADER_COMMAND_AVAILABLE_WIFI_CONNECT |
            H2_LOADER_COMMAND_AVAILABLE_WIFI_DISCONNECT |
            H2_LOADER_COMMAND_AVAILABLE_LOADER_UPGRADE |
            H2_LOADER_COMMAND_AVAILABLE_COREDUMP_STATUS |
            H2_LOADER_COMMAND_AVAILABLE_COREDUMP_DUMP |
            H2_LOADER_COMMAND_AVAILABLE_COREDUMP_ERASE;
        if (config->memory_stats.read != NULL) {
            implemented |= H2_LOADER_COMMAND_AVAILABLE_MEMORY;
        }
        rc = h2_loader_set_implemented_commands(config->loader, implemented);
    }
    return rc;
}

int h2_loader_command_execute(
    h2_loader_command_t *self,
    size_t argc,
    const char *const *argv) {
    if (self == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_command_execute(&self->command, argc, argv);
}

int h2_loader_command_poll(h2_loader_command_t *self, uint32_t timeout_ms) {
    if (self == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_command_poll(&self->command, timeout_ms);
}
