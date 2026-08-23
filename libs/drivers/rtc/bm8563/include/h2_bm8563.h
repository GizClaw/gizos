#ifndef H2_BM8563_H
#define H2_BM8563_H

#include "h2_bm8563_transport.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BM8563_REGISTER_CONTROL_STATUS_1 0x00u
#define H2_BM8563_REGISTER_VL_SECONDS 0x02u
#define H2_BM8563_TIME_REGISTER_COUNT 7u

/** Calendar with Bluetooth Current Time weekday numbering. */
typedef struct h2_bm8563_calendar {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday; /**< Monday is 1 and Sunday is 7. */
} h2_bm8563_calendar_t;

typedef struct h2_bm8563_config {
    h2_bm8563_transport_t transport;
    /**
     * First of two representable centuries, aligned to 100 years.
     * A set RTC century bit selects this value; a clear bit selects this value
     * plus 100.
     */
    uint16_t century_base_year;
} h2_bm8563_config_t;

typedef struct h2_bm8563 {
    h2_bm8563_transport_t transport;
    uint16_t century_base_year;
    uint8_t initialized;
} h2_bm8563_t;

h2_pal_result_t h2_bm8563_init(h2_bm8563_t *rtc,
                               const h2_bm8563_config_t *config);

void h2_bm8563_deinit(h2_bm8563_t *rtc);

h2_pal_result_t
h2_bm8563_validate_calendar(const h2_bm8563_t *rtc,
                            const h2_bm8563_calendar_t *calendar);

h2_pal_result_t h2_bm8563_read_calendar(h2_bm8563_t *rtc,
                                        h2_bm8563_calendar_t *out_calendar);

h2_pal_result_t h2_bm8563_write_calendar(h2_bm8563_t *rtc,
                                         const h2_bm8563_calendar_t *calendar);

h2_pal_result_t
h2_bm8563_calendar_to_unix_ms(const h2_bm8563_calendar_t *calendar,
                              uint64_t *out_unix_ms);

h2_pal_result_t
h2_bm8563_unix_ms_to_calendar(uint64_t unix_ms,
                              h2_bm8563_calendar_t *out_calendar);

h2_pal_result_t h2_bm8563_get_unix_ms(h2_bm8563_t *rtc, uint64_t *out_unix_ms);

/** Set wall time at the RTC's one-second resolution (milliseconds truncate). */
h2_pal_result_t h2_bm8563_set_unix_ms(h2_bm8563_t *rtc, uint64_t unix_ms);

#ifdef __cplusplus
}
#endif

#endif
