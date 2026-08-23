#include "h2_linux_serial_host.h"
#include "h2_posix_serial_host_internal.h"

#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int copy_text(char *out, size_t out_size, const char *value) {
    size_t length;
    if (out == NULL || out_size == 0u || value == NULL) {
        return 0;
    }
    length = strlen(value);
    if (length == 0u || length >= out_size) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, value, length + 1u);
    return 1;
}

static int read_text_file(
    const char *directory,
    const char *name,
    char *out,
    size_t out_size) {
    char path[PATH_MAX];
    FILE *file;
    size_t length;
    const int path_length =
        snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        return 0;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }
    if (fgets(out, (int)out_size, file) == NULL) {
        fclose(file);
        return 0;
    }
    fclose(file);
    length = strlen(out);
    while (length > 0u &&
           (out[length - 1u] == '\n' || out[length - 1u] == '\r')) {
        out[--length] = '\0';
    }
    return length > 0u;
}

static int find_sysfs_value(
    const char *tty_name,
    const char *name,
    char *out,
    size_t out_size) {
    char link_path[PATH_MAX];
    char current[PATH_MAX];
    const int path_length = snprintf(
            link_path,
            sizeof(link_path),
            "/sys/class/tty/%s/device",
            tty_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(link_path) ||
        realpath(link_path, current) == NULL) {
        return 0;
    }
    while (strncmp(current, "/sys/", strlen("/sys/")) == 0) {
        char *slash;
        if (read_text_file(current, name, out, out_size)) {
            return 1;
        }
        slash = strrchr(current, '/');
        if (slash == NULL || slash == current) {
            break;
        }
        *slash = '\0';
    }
    return 0;
}

static int contains_port_id(
    const h2_pal_serial_host_snapshot_t *snapshot,
    const char *port_id) {
    size_t index;
    for (index = 0u; index < snapshot->count; ++index) {
        if (strcmp(snapshot->ports[index].port_id, port_id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int append_port(
    h2_pal_serial_host_snapshot_t *snapshot,
    const h2_pal_serial_host_port_info_t *info) {
    h2_pal_serial_host_port_info_t *ports = realloc(
        snapshot->ports,
        (snapshot->count + 1u) * sizeof(*ports));
    if (ports == NULL) {
        return 0;
    }
    snapshot->ports = ports;
    snapshot->ports[snapshot->count] = *info;
    snapshot->count += 1u;
    return 1;
}

static int compare_ports(const void *lhs, const void *rhs) {
    const h2_pal_serial_host_port_info_t *left =
        (const h2_pal_serial_host_port_info_t *)lhs;
    const h2_pal_serial_host_port_info_t *right =
        (const h2_pal_serial_host_port_info_t *)rhs;
    return strcmp(left->endpoint, right->endpoint);
}

static int add_candidate(
    h2_pal_serial_host_snapshot_t *snapshot,
    const char *endpoint) {
    h2_pal_serial_host_port_info_t info = {0};
    char canonical[PATH_MAX];
    char metadata[256];
    const char *tty_name;
    unsigned long parsed;
    char *end;

    if (realpath(endpoint, canonical) == NULL ||
        !copy_text(info.port_id, sizeof(info.port_id), canonical) ||
        !copy_text(info.endpoint, sizeof(info.endpoint), endpoint) ||
        contains_port_id(snapshot, info.port_id)) {
        return 1;
    }
    tty_name = strrchr(canonical, '/');
    tty_name = tty_name == NULL ? canonical : tty_name + 1;
    info.capabilities =
        H2_PAL_SERIAL_HOST_CAP_DTR | H2_PAL_SERIAL_HOST_CAP_RTS;
    if (find_sysfs_value(
            tty_name,
            "product",
            info.display_name,
            sizeof(info.display_name))) {
        info.valid_fields |= H2_PAL_SERIAL_HOST_PORT_FIELD_DISPLAY_NAME;
    }
    if (find_sysfs_value(tty_name, "serial", info.usb_serial, sizeof(info.usb_serial))) {
        info.valid_fields |= H2_PAL_SERIAL_HOST_PORT_FIELD_USB_SERIAL;
    }
    if (find_sysfs_value(tty_name, "idVendor", metadata, sizeof(metadata))) {
        errno = 0;
        parsed = strtoul(metadata, &end, 16);
        if (errno == 0 && end != metadata && *end == '\0' &&
            parsed <= UINT16_MAX) {
            info.usb_vid = (uint16_t)parsed;
            info.valid_fields |= H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID;
        }
    }
    if (find_sysfs_value(tty_name, "idProduct", metadata, sizeof(metadata))) {
        errno = 0;
        parsed = strtoul(metadata, &end, 16);
        if (errno == 0 && end != metadata && *end == '\0' &&
            parsed <= UINT16_MAX) {
            info.usb_pid = (uint16_t)parsed;
            info.valid_fields |= H2_PAL_SERIAL_HOST_PORT_FIELD_USB_PID;
        }
    }
    return append_port(snapshot, &info);
}

h2_pal_result_t h2_posix_serial_host_scan_native(
    h2_pal_serial_host_snapshot_t **out_snapshot) {
    static const char *patterns[] = {
        "/dev/serial/by-id/*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
    };
    h2_pal_serial_host_snapshot_t *snapshot;
    glob_t matches = {0};
    size_t pattern_index;
    size_t match_index;
    int flags = 0;

    if (out_snapshot == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_snapshot = NULL;
    snapshot = calloc(1u, sizeof(*snapshot));
    if (snapshot == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    for (pattern_index = 0u;
         pattern_index < sizeof(patterns) / sizeof(patterns[0]);
         ++pattern_index) {
        const int result = glob(patterns[pattern_index], flags, NULL, &matches);
        if (result != 0 && result != GLOB_NOMATCH) {
            globfree(&matches);
            free(snapshot);
            return H2_PAL_ERR_IO;
        }
        if (result == 0) {
            flags = GLOB_APPEND;
        }
    }
    for (match_index = 0u; match_index < matches.gl_pathc; ++match_index) {
        if (!add_candidate(snapshot, matches.gl_pathv[match_index])) {
            globfree(&matches);
            free(snapshot->ports);
            free(snapshot);
            return H2_PAL_ERR_NO_MEMORY;
        }
    }
    globfree(&matches);
    if (snapshot->count > 1u) {
        qsort(
            snapshot->ports,
            snapshot->count,
            sizeof(*snapshot->ports),
            compare_ports);
    }
    *out_snapshot = snapshot;
    return H2_PAL_OK;
}

const h2_pal_serial_host_api_t *h2_linux_serial_host_api(void) {
    return h2_posix_serial_host_api();
}
