#include "h2_bk_platform_core.h"
#include "driver/aon_rtc.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
static uint64_t now_us = UINT64_C(4294967296) * 1000u + 123u;
static struct timeval wall;
uint64_t bk_aon_rtc_get_us(void) { return now_us; }
int bk_rtc_gettimeofday(struct timeval *out, void *zone) { (void)zone; *out=wall; return 0; }
int bk_rtc_settimeofday(const struct timeval *value, const void *zone) { (void)zone; wall=*value; return 0; }
int rtos_delay_milliseconds(uint32_t ms) { now_us+=(uint64_t)ms*1000u; return 0; }
int main(void) {
    const h2_pal_time_api_t *api=h2_bk_platform_time_api();
    uint64_t ms,us,out=99;
    assert(h2_pal_time_get_monotonic_ms(api,&ms)==H2_PAL_OK);
    assert(h2_pal_time_get_monotonic_us(api,&us)==H2_PAL_OK);
    assert(ms==us/1000u && ms==UINT64_C(4294967296));
    assert(h2_pal_time_get_wall_ms(api,&out)==H2_PAL_TIME_ERR_UNCALIBRATED && out==0);
    assert(h2_pal_time_set_wall_ms(api,UINT64_C(1790534076123))==H2_PAL_OK);
    assert(h2_pal_time_get_wall_ms(api,&out)==H2_PAL_OK && out==UINT64_C(1790534076123));
    assert(now_us==us);
    assert(h2_pal_time_sleep_ms(api,20)==H2_PAL_OK);
    assert(h2_pal_time_get_monotonic_ms(api,&ms)==H2_PAL_OK && ms==(us+20000u)/1000u);
    assert(h2_pal_time_get_monotonic_ms(api,NULL)==H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_time_set_wall_ms(api,0)==H2_PAL_ERR_INVALID_ARG);
    wall.tv_sec=-1;
    assert(h2_pal_time_get_wall_ms(api,&out)==H2_PAL_ERR_UNAVAILABLE && out==0);
    return 0;
}
