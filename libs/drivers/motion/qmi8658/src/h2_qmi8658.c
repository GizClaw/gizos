#include "h2_qmi8658.h"

#include <stddef.h>
#include <string.h>

#define H2_QMI8658_CTRL1_ADDR_AUTO_INCREMENT 0x40u
#define H2_QMI8658_CTRL7_ENABLE_ACCEL_GYRO 0x03u
#define H2_QMI8658_CTRL2_ACCEL_4G_250HZ 0x15u
#define H2_QMI8658_CTRL3_GYRO_512DPS_250HZ 0x55u
#define H2_QMI8658_RESET_CMD 0xb0u

static h2_pal_result_t read_reg(h2_qmi8658_t *imu, uint8_t reg, uint8_t *out) {
    return imu->transport.read_regs(imu->transport.user, reg, out, 1u);
}

static int16_t le_i16(const uint8_t *bytes) {
    return (int16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int transport_valid(const h2_qmi8658_transport_t *transport) {
    return transport->write_reg != NULL &&
           transport->read_regs != NULL &&
           transport->sleep_ms != NULL;
}

h2_pal_result_t h2_qmi8658_init(h2_qmi8658_t *imu, h2_qmi8658_transport_t transport) {
    if (imu == NULL || !transport_valid(&transport)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(imu, 0, sizeof(*imu));
    imu->transport = transport;
    return H2_PAL_OK;
}

h2_pal_result_t h2_qmi8658_open(h2_qmi8658_t *imu) {
    if (imu == NULL || !transport_valid(&imu->transport)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (imu->opened) {
        return H2_PAL_OK;
    }

    uint8_t who_am_i = 0;
    h2_pal_result_t rc = read_reg(imu, H2_QMI8658_REG_WHO_AM_I, &who_am_i);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (who_am_i != H2_QMI8658_WHO_AM_I_VALUE) {
        return H2_PAL_ERR_NOT_FOUND;
    }

    rc = imu->transport.write_reg(imu->transport.user, H2_QMI8658_REG_RESET, H2_QMI8658_RESET_CMD);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    imu->transport.sleep_ms(imu->transport.user, 10u);

    rc = imu->transport.write_reg(
        imu->transport.user,
        H2_QMI8658_REG_CTRL1,
        H2_QMI8658_CTRL1_ADDR_AUTO_INCREMENT);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = imu->transport.write_reg(
        imu->transport.user,
        H2_QMI8658_REG_CTRL7,
        H2_QMI8658_CTRL7_ENABLE_ACCEL_GYRO);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = imu->transport.write_reg(
        imu->transport.user,
        H2_QMI8658_REG_CTRL2,
        H2_QMI8658_CTRL2_ACCEL_4G_250HZ);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = imu->transport.write_reg(
        imu->transport.user,
        H2_QMI8658_REG_CTRL3,
        H2_QMI8658_CTRL3_GYRO_512DPS_250HZ);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    imu->opened = 1;
    return H2_PAL_OK;
}

h2_pal_result_t h2_qmi8658_read_sample(h2_qmi8658_t *imu, h2_qmi8658_sample_t *out) {
    if (imu == NULL || out == NULL || !transport_valid(&imu->transport)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!imu->opened) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    uint8_t data[12] = { 0 };
    h2_pal_result_t rc = imu->transport.read_regs(
        imu->transport.user,
        H2_QMI8658_REG_AX_L,
        data,
        sizeof(data));
    if (rc != H2_PAL_OK) {
        return rc;
    }

    const int16_t ax = le_i16(&data[0]);
    const int16_t ay = le_i16(&data[2]);
    const int16_t az = le_i16(&data[4]);
    const int16_t gx = le_i16(&data[6]);
    const int16_t gy = le_i16(&data[8]);
    const int16_t gz = le_i16(&data[10]);

    out->accel_mg[0] = ((int32_t)ax * 1000) / 8192;
    out->accel_mg[1] = ((int32_t)ay * 1000) / 8192;
    out->accel_mg[2] = ((int32_t)az * 1000) / 8192;
    out->gyro_mdps[0] = ((int32_t)gx * 1000) / 64;
    out->gyro_mdps[1] = ((int32_t)gy * 1000) / 64;
    out->gyro_mdps[2] = ((int32_t)gz * 1000) / 64;
    return H2_PAL_OK;
}
