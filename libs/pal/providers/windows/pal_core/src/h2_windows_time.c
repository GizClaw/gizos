#include "h2_windows_internal.h"

#include <limits.h>

#define H2_WINDOWS_FILETIME_UNIX_EPOCH UINT64_C(116444736000000000)

h2_pal_result_t h2_windows_monotonic_ms(
    h2_windows_platform_t *platform,
    uint64_t *out_ms) {
    LARGE_INTEGER counter;
    if (out_ms == NULL || !QueryPerformanceCounter(&counter) ||
        counter.QuadPart < 0) {
        return H2_PAL_ERR_IO;
    }
    uint64_t value = (uint64_t)counter.QuadPart;
    uint64_t frequency = (uint64_t)platform->qpc_frequency.QuadPart;
    *out_ms = (value / frequency) * UINT64_C(1000) +
              ((value % frequency) * UINT64_C(1000)) / frequency;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_time_monotonic(void *user, uint64_t *out_ms) {
    return h2_windows_monotonic_ms(user, out_ms);
}

static h2_pal_result_t windows_time_wall(void *user, uint64_t *out_ms) {
    (void)user;
    if (out_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    FILETIME value;
    GetSystemTimePreciseAsFileTime(&value);
    ULARGE_INTEGER ticks;
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    if (ticks.QuadPart < H2_WINDOWS_FILETIME_UNIX_EPOCH) {
        return H2_PAL_ERR_IO;
    }
    *out_ms = (ticks.QuadPart - H2_WINDOWS_FILETIME_UNIX_EPOCH) /
              UINT64_C(10000);
    return H2_PAL_OK;
}

static h2_pal_result_t windows_time_set_wall(void *user, uint64_t wall_ms) {
    (void)user;
    if (wall_ms > (UINT64_MAX - H2_WINDOWS_FILETIME_UNIX_EPOCH) /
                      UINT64_C(10000)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    ULARGE_INTEGER ticks;
    ticks.QuadPart = wall_ms * UINT64_C(10000) +
                     H2_WINDOWS_FILETIME_UNIX_EPOCH;
    FILETIME file_time;
    file_time.dwLowDateTime = ticks.LowPart;
    file_time.dwHighDateTime = ticks.HighPart;
    SYSTEMTIME system_time;
    if (!FileTimeToSystemTime(&file_time, &system_time)) {
        return h2_windows_error_from_win32(GetLastError());
    }
    if (!SetSystemTime(&system_time)) {
        return h2_windows_error_from_win32(GetLastError());
    }
    return H2_PAL_OK;
}

static h2_pal_result_t windows_time_wall_status(
    void *user,
    h2_pal_time_wall_status_t *out_status) {
    (void)user;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_status = (h2_pal_time_wall_status_t){
        .valid = 1u,
        .source = H2_PAL_TIME_WALL_SOURCE_RTC,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t windows_time_sleep(void *user, uint32_t ms) {
    (void)user;
    Sleep(ms);
    return H2_PAL_OK;
}

const h2_pal_time_vtable_t h2_windows_time_vtable = {
    .get_monotonic_ms = windows_time_monotonic,
    .get_wall_ms = windows_time_wall,
    .set_wall_ms = windows_time_set_wall,
    .get_wall_status = windows_time_wall_status,
    .sleep_ms = windows_time_sleep,
};
