#ifndef TEST_BK_AON_RTC_H
#define TEST_BK_AON_RTC_H
#include <stdint.h>
#include <sys/time.h>
uint64_t bk_aon_rtc_get_us(void);
int bk_rtc_gettimeofday(struct timeval *, void *);
int bk_rtc_settimeofday(const struct timeval *, const void *);
#endif
