#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_imu_read_imu(void *p0, h2_pal_periph_id_t p1, h2_pal_imu_reading_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_imu_vtable_t unsupported_imu_vtable = {
    .read_imu = unsupported_imu_read_imu,
};
static const h2_pal_imu_api_t unsupported_imu_api = { .user = NULL, .vtable = &unsupported_imu_vtable };
const h2_pal_imu_api_t *h2_pal_unsupported_imu_api(void) { return &unsupported_imu_api; }
