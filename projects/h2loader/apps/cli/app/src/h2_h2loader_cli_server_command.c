#include "h2_h2loader_cli_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static h2_pal_result_t cli_server_connect(
    void *user,
    uint32_t command_timeout_ms,
    h2_h2loader_host_status_t *out_status) {
    h2_h2loader_cli_transport_t *transport = user;
    transport->command_timeout_ms = command_timeout_ms;
    return h2_h2loader_cli_transport_connect(transport, out_status);
}

static h2_pal_result_t cli_server_execute(
    void *user,
    const h2_h2loader_host_command_request_t *request,
    h2_h2loader_host_command_result_t *out_result) {
    return h2_h2loader_cli_transport_execute(user, request, out_result);
}

static h2_pal_result_t cli_server_disconnect(void *user) {
    return h2_h2loader_cli_transport_disconnect(user);
}

static h2_pal_result_t cli_server_rediscover(void *user) {
    return h2_h2loader_cli_transport_rediscover(user);
}

static const h2_h2loader_cli_server_transport_vtable_t
    cli_server_transport_vtable = {
        .connect = cli_server_connect,
        .execute = cli_server_execute,
        .disconnect = cli_server_disconnect,
        .rediscover = cli_server_rediscover,
    };

static int parse_u64(const char *value, uint64_t *out) {
    char *end = NULL;
    unsigned long long parsed;
    if (value == NULL || value[0] == '\0' || value[0] == '-') return 0;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0u) return 0;
    *out = (uint64_t)parsed;
    return 1;
}

static int parse_seconds(const char *value, uint32_t *out_ms) {
    char *end = NULL;
    double parsed;
    if (value == NULL || value[0] == '\0' || value[0] == '-') return 0;
    errno = 0;
    parsed = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed <= 0.0 || parsed > 3600.0) {
        return 0;
    }
    *out_ms = (uint32_t)(parsed * 1000.0);
    return *out_ms != 0u;
}

static int valid_sha256(const char *value) {
    if (value == NULL || strlen(value) != H2_H2LOADER_HOST_SHA256_HEX_LEN) return 0;
    for (size_t i = 0u; i < H2_H2LOADER_HOST_SHA256_HEX_LEN; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return 0;
    }
    return 1;
}

int h2_h2loader_cli_server_options_parse(
    int argc,
    const char *const *argv,
    h2_h2loader_cli_server_options_t *out) {
    if (out == NULL) return 0;
    *out = (h2_h2loader_cli_server_options_t){
        .bind_host = "0.0.0.0",
        .url_path = "/update.tar.zlib",
        .download_timeout_ms = 660000u,
    };
    for (int i = 0; i < argc; ++i) {
        uint64_t parsed;
        const char *option = argv[i];
        if (i + 1 >= argc) return 0;
        const char *value = argv[++i];
        if (strcmp(option, "--url") == 0) out->url = value;
        else if (strcmp(option, "--file") == 0) out->file_path = value;
        else if (strcmp(option, "--bytes") == 0) {
            if (!parse_u64(value, &out->expected_bytes)) return 0;
        } else if (strcmp(option, "--sha256") == 0) out->expected_sha256 = value;
        else if (strcmp(option, "--host") == 0) out->bind_host = value;
        else if (strcmp(option, "--url-host") == 0) out->url_host = value;
        else if (strcmp(option, "--url-path") == 0) out->url_path = value;
        else if (strcmp(option, "--ssid") == 0) out->ssid = value;
        else if (strcmp(option, "--password") == 0) out->password = value;
        else if (strcmp(option, "--http-port") == 0) {
            if (!parse_u64(value, &parsed) || parsed > UINT16_MAX) {
                if (strcmp(value, "0") != 0) return 0;
                parsed = 0u;
            }
            out->port = (uint16_t)parsed;
        } else if (strcmp(option, "--download-timeout") == 0) {
            if (!parse_seconds(value, &out->download_timeout_ms)) return 0;
        } else return 0;
    }
    if ((out->url == NULL) == (out->file_path == NULL) ||
        (out->ssid == NULL) != (out->password == NULL)) {
        return 0;
    }
    return out->file_path != NULL ||
        (out->expected_bytes != 0u && valid_sha256(out->expected_sha256));
}

static h2_pal_result_t server_command_output(
    void *user,
    const uint8_t *data,
    size_t len) {
    h2_h2loader_cli_context_t *context = user;
    return h2_h2loader_cli_output_bytes(
        context, H2_H2LOADER_CLI_STREAM_STDOUT, data, len);
}

int h2_h2loader_cli_server_command_with_transport(
    h2_h2loader_cli_context_t *context,
    const h2_h2loader_cli_options_t *options,
    int argc,
    const char *const *argv,
    const h2_h2loader_cli_server_transport_api_t *transport) {
    h2_h2loader_cli_server_options_t parsed;
    h2_h2loader_host_status_t status = {0};
    h2_h2loader_host_command_result_t result = {0};
    h2_pal_result_t rc = H2_PAL_OK;
    if (context == NULL || context->config == NULL ||
        context->runtime == NULL || options == NULL ||
        (argc != 0 && argv == NULL) || transport == NULL ||
        transport->vtable == NULL || transport->vtable->connect == NULL ||
        transport->vtable->execute == NULL ||
        transport->vtable->disconnect == NULL ||
        transport->vtable->rediscover == NULL) {
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    if (options->port == NULL ||
        !h2_h2loader_cli_server_options_parse(argc, argv, &parsed)) {
        return H2_H2LOADER_CLI_EXIT_USAGE;
    }
    if (parsed.file_path != NULL) {
        h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDERR,
            "h2loader: send-url --file is unsupported because PAL Net has no TCP listener\n");
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    if (context->config->is_cancelled != NULL &&
        context->config->is_cancelled(context->config->cancel_user)) {
        rc = H2_PAL_EXIT;
        goto cleanup;
    }
    rc = transport->vtable->connect(
        transport->user, parsed.download_timeout_ms, &status);
    if (rc == H2_PAL_OK && parsed.ssid != NULL) {
        h2_h2loader_host_command_request_t wifi = {
            .command = H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT,
            .status = &status,
            .ssid = parsed.ssid,
            .password = parsed.password,
            .is_cancelled = context->config->is_cancelled,
            .cancel_user = context->config->cancel_user,
            .on_output = server_command_output,
            .output_user = context,
        };
        rc = transport->vtable->execute(transport->user, &wifi, &result);
        if (rc == H2_PAL_OK &&
            result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
            rc = H2_PAL_ERR_IO;
        }
    }
    if (rc == H2_PAL_OK) {
        h2_h2loader_host_command_request_t stage = {
            .command = H2_H2LOADER_HOST_COMMAND_STAGE_URL,
            .status = &status,
            .url = parsed.url,
            .expected_bytes = parsed.expected_bytes,
            .expected_sha256 = parsed.expected_sha256,
            .is_cancelled = context->config->is_cancelled,
            .cancel_user = context->config->cancel_user,
            .on_output = server_command_output,
            .output_user = context,
        };
        rc = transport->vtable->execute(transport->user, &stage, &result);
        if (rc == H2_PAL_OK &&
            result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
            rc = H2_PAL_ERR_IO;
        }
    }
    (void)transport->vtable->disconnect(transport->user);
    if (rc == H2_PAL_OK) rc = transport->vtable->rediscover(transport->user);
    if (rc == H2_PAL_OK) rc = transport->vtable->connect(
        transport->user, parsed.download_timeout_ms, &status);
    if (rc == H2_PAL_OK &&
        (status.staged_valid == 0u ||
         status.staged_bytes != parsed.expected_bytes ||
         strcmp(status.staged_checksum, parsed.expected_sha256) != 0)) {
        rc = H2_PAL_ERR_INVALID_STATE;
    }
cleanup:
    (void)transport->vtable->disconnect(transport->user);
    if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
    h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDOUT,
        "H2_LOADER_SEND_URL result=OK bytes=%llu checksum=%s\n",
        (unsigned long long)parsed.expected_bytes,
        parsed.expected_sha256);
    return H2_H2LOADER_CLI_EXIT_OK;
}

int h2_h2loader_cli_server_command(
    h2_h2loader_cli_context_t *context,
    const h2_h2loader_cli_options_t *options,
    int argc,
    const char *const *argv) {
    h2_h2loader_cli_transport_t session;
    h2_h2loader_cli_transport_init(
        &session, context, options, options->read_timeout_ms);
    h2_h2loader_cli_server_transport_api_t transport = {
        .user = &session,
        .vtable = &cli_server_transport_vtable,
    };
    return h2_h2loader_cli_server_command_with_transport(
        context, options, argc, argv, &transport);
}
