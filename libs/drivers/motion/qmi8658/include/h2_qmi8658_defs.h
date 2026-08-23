#ifndef H2_QMI8658_DEFS_H
#define H2_QMI8658_DEFS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_QMI8658_I2C_ADDR_SA0_LOW 0x6au
#define H2_QMI8658_I2C_ADDR_SA0_HIGH 0x6bu
#define H2_QMI8658_WHO_AM_I_VALUE 0x05u

typedef enum h2_qmi8658_register {
    H2_QMI8658_REG_WHO_AM_I = 0x00,
    H2_QMI8658_REG_CTRL1 = 0x02,
    H2_QMI8658_REG_CTRL2 = 0x03,
    H2_QMI8658_REG_CTRL3 = 0x04,
    H2_QMI8658_REG_CTRL7 = 0x08,
    H2_QMI8658_REG_AX_L = 0x35,
    H2_QMI8658_REG_RESET = 0x60,
} h2_qmi8658_register_t;

#ifdef __cplusplus
}
#endif

#endif
