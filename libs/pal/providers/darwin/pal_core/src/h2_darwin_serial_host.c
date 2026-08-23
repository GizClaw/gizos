#include "h2_darwin_platform.h"
#include "h2_posix_serial_host_internal.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int copy_cf_string(CFTypeRef value, char *out, size_t out_size) {
    if (value == NULL || CFGetTypeID(value) != CFStringGetTypeID() ||
        out == NULL || out_size == 0u) {
        return 0;
    }
    if (!CFStringGetCString(
            (CFStringRef)value,
            out,
            (CFIndex)out_size,
            kCFStringEncodingUTF8)) {
        out[0] = '\0';
        return 0;
    }
    return out[0] != '\0';
}

static CFTypeRef search_property(io_registry_entry_t entry, CFStringRef key) {
    return IORegistryEntrySearchCFProperty(
        entry,
        kIOServicePlane,
        key,
        kCFAllocatorDefault,
        kIORegistryIterateRecursively | kIORegistryIterateParents);
}

static int copy_string_property(
    io_registry_entry_t entry,
    CFStringRef key,
    char *out,
    size_t out_size) {
    CFTypeRef value = search_property(entry, key);
    int copied = copy_cf_string(value, out, out_size);
    if (value != NULL) {
        CFRelease(value);
    }
    return copied;
}

static int copy_u16_property(
    io_registry_entry_t entry,
    CFStringRef key,
    uint16_t *out) {
    CFTypeRef value = search_property(entry, key);
    int32_t number = 0;
    int copied = 0;
    if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID() &&
        CFNumberGetValue(
            (CFNumberRef)value,
            kCFNumberSInt32Type,
            &number) &&
        number >= 0 && number <= UINT16_MAX) {
        *out = (uint16_t)number;
        copied = 1;
    }
    if (value != NULL) {
        CFRelease(value);
    }
    return copied;
}

static int contains_endpoint(
    const h2_pal_serial_host_snapshot_t *snapshot,
    const char *endpoint) {
    size_t index;
    for (index = 0u; index < snapshot->count; ++index) {
        if (strcmp(snapshot->ports[index].endpoint, endpoint) == 0) {
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

h2_pal_result_t h2_posix_serial_host_scan_native(
    h2_pal_serial_host_snapshot_t **out_snapshot) {
    h2_pal_serial_host_snapshot_t *snapshot;
    CFMutableDictionaryRef matching;
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kernel_result;
    io_object_t service;

    if (out_snapshot == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_snapshot = NULL;
    snapshot = calloc(1u, sizeof(*snapshot));
    if (snapshot == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    matching = IOServiceMatching(kIOSerialBSDServiceValue);
    if (matching == NULL) {
        free(snapshot);
        return H2_PAL_ERR_NO_MEMORY;
    }
    CFDictionarySetValue(
        matching,
        CFSTR(kIOSerialBSDTypeKey),
        CFSTR(kIOSerialBSDAllTypes));
    kernel_result =
        IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
    if (kernel_result != KERN_SUCCESS) {
        free(snapshot);
        return H2_PAL_ERR_IO;
    }

    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        h2_pal_serial_host_port_info_t info = {0};
        CFTypeRef callout = IORegistryEntryCreateCFProperty(
            service,
            CFSTR(kIOCalloutDeviceKey),
            kCFAllocatorDefault,
            0);
        const int has_callout = copy_cf_string(
            callout,
            info.endpoint,
            sizeof(info.endpoint));
        if (callout != NULL) {
            CFRelease(callout);
        }
        if (!has_callout ||
            strncmp(info.endpoint, "/dev/cu.", strlen("/dev/cu.")) != 0 ||
            contains_endpoint(snapshot, info.endpoint)) {
            IOObjectRelease(service);
            continue;
        }
        memcpy(info.port_id, info.endpoint, strlen(info.endpoint) + 1u);
        if (copy_string_property(
                service,
                CFSTR("USB Product Name"),
                info.display_name,
                sizeof(info.display_name)) ||
            copy_string_property(
                service,
                CFSTR(kIOTTYDeviceKey),
                info.display_name,
                sizeof(info.display_name))) {
            info.valid_fields |= H2_PAL_SERIAL_HOST_PORT_FIELD_DISPLAY_NAME;
        }
        if (copy_u16_property(
                service,
                CFSTR("idVendor"),
                &info.usb_vid)) {
            info.valid_fields |= H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID;
        }
        if (copy_u16_property(
                service,
                CFSTR("idProduct"),
                &info.usb_pid)) {
            info.valid_fields |= H2_PAL_SERIAL_HOST_PORT_FIELD_USB_PID;
        }
        if ((info.valid_fields &
             (H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID |
              H2_PAL_SERIAL_HOST_PORT_FIELD_USB_PID)) ==
            (H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID |
             H2_PAL_SERIAL_HOST_PORT_FIELD_USB_PID)) {
            info.capabilities =
                H2_PAL_SERIAL_HOST_CAP_DTR | H2_PAL_SERIAL_HOST_CAP_RTS;
        }
        if (copy_string_property(
                service,
                CFSTR("USB Serial Number"),
                info.usb_serial,
                sizeof(info.usb_serial))) {
            info.valid_fields |= H2_PAL_SERIAL_HOST_PORT_FIELD_USB_SERIAL;
        }
        if (!append_port(snapshot, &info)) {
            IOObjectRelease(service);
            IOObjectRelease(iterator);
            free(snapshot->ports);
            free(snapshot);
            return H2_PAL_ERR_NO_MEMORY;
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
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

const h2_pal_serial_host_api_t *h2_darwin_serial_host_api(void) {
    return h2_posix_serial_host_api();
}
