#ifndef H2_QMI8658_H
#define H2_QMI8658_H

#include <stdint.h>

#include "h2/pal/core/h2_pal_errors.h"
#include "h2_qmi8658_defs.h"
#include "h2_qmi8658_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_qmi8658 {
    h2_qmi8658_transport_t transport;
    int opened;
} h2_qmi8658_t;

typedef struct h2_qmi8658_sample {
    int32_t accel_mg[3];
    int32_t gyro_mdps[3];
} h2_qmi8658_sample_t;

h2_pal_result_t h2_qmi8658_init(h2_qmi8658_t *imu, h2_qmi8658_transport_t transport);
h2_pal_result_t h2_qmi8658_open(h2_qmi8658_t *imu);
h2_pal_result_t h2_qmi8658_read_sample(h2_qmi8658_t *imu, h2_qmi8658_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif
