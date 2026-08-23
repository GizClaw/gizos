#include <time.h>

/*
 * ESP-IDF 6.0's picolibc headers declare timespec_get(), but its ESP32-S3
 * archive does not provide the symbol. Keep this weak so a later libc
 * implementation takes precedence without changing the GizClaw SDK source.
 * TODO(#430): Remove this shim after Firmwares consumes the upstream fix.
 */
__attribute__((weak)) int timespec_get(struct timespec *time_point, int base) {
    if (time_point == NULL || base != TIME_UTC) {
        return 0;
    }
    return clock_gettime(CLOCK_REALTIME, time_point) == 0 ? TIME_UTC : 0;
}
