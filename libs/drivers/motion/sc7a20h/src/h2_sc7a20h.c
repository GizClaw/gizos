#include "h2_sc7a20h.h"

#include <string.h>

/* Silan SC7A20H manual v1.1, sections 11.1 and 13.2--13.14/13.37. */
#define H2_SC7A20H_REG_WHO_AM_I 0x0fu
#define H2_SC7A20H_REG_MODE 0x1fu
#define H2_SC7A20H_REG_CTRL1 0x20u
#define H2_SC7A20H_REG_CTRL2 0x21u
#define H2_SC7A20H_REG_CTRL3 0x22u
#define H2_SC7A20H_REG_CTRL4 0x23u
#define H2_SC7A20H_REG_CTRL5 0x24u
#define H2_SC7A20H_REG_CTRL6 0x25u
#define H2_SC7A20H_REG_STATUS 0x27u
#define H2_SC7A20H_REG_XYZ_BURST 0xa8u
#define H2_SC7A20H_REG_FIFO_CTRL 0x2eu
#define H2_SC7A20H_REG_VERSION 0x70u
#define H2_SC7A20H_XYZ_ENABLE 0x07u

struct h2_sc7a20h {
    h2_sc7a20h_config_t config;
    int opened;
    int powerdown_required;
};

static int config_valid(const h2_sc7a20h_config_t *config) {
    return config != NULL && config->mem != NULL && config->mem->vtable != NULL &&
           config->mem->vtable->alloc != NULL && config->mem->vtable->free != NULL &&
           config->transport.write_reg != NULL && config->transport.read_regs != NULL &&
           config->transport.sleep_ms != NULL &&
           (uint32_t)config->range <= (uint32_t)H2_SC7A20H_RANGE_16G &&
           config->odr >= H2_SC7A20H_ODR_1_56HZ && config->odr <= H2_SC7A20H_ODR_4434HZ &&
           config->data_ready_int1 <= 1u;
}

static h2_pal_result_t read_reg(h2_sc7a20h_t *device, uint8_t reg, uint8_t *out) {
    return device->config.transport.read_regs(device->config.transport.user, reg, out, 1u);
}

static h2_pal_result_t write_reg(h2_sc7a20h_t *device, uint8_t reg, uint8_t value) {
    return device->config.transport.write_reg(device->config.transport.user, reg, value);
}

h2_pal_result_t h2_sc7a20h_create(const h2_sc7a20h_config_t *config, h2_sc7a20h_t **out_device) {
    if (out_device == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_device = NULL;
    if (!config_valid(config)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_sc7a20h_t *device = h2_pal_mem_alloc(config->mem, sizeof(*device));
    if (device == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(device, 0, sizeof(*device));
    device->config = *config;
    *out_device = device;
    return H2_PAL_OK;
}

h2_pal_result_t h2_sc7a20h_open(h2_sc7a20h_t *device) {
    if (device == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (device->opened) {
        return H2_PAL_OK;
    }
    uint8_t chip = 0u;
    uint8_t version = 0u;
    h2_pal_result_t rc = read_reg(device, H2_SC7A20H_REG_WHO_AM_I, &chip);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (chip != 0x11u) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    rc = read_reg(device, H2_SC7A20H_REG_VERSION, &version);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (version != 0x28u) {
        return H2_PAL_ERR_NOT_FOUND;
    }

    const uint8_t settings[][2] = {
        {H2_SC7A20H_REG_CTRL1, H2_SC7A20H_XYZ_ENABLE},
        {H2_SC7A20H_REG_MODE, 0x01u},  /* HR is in MODE, not CTRL4. */
        {H2_SC7A20H_REG_CTRL2, 0x00u}, /* Preserve the gravity/DC component. */
        {H2_SC7A20H_REG_CTRL3, device->config.data_ready_int1 ? 0x10u : 0x00u},
        {H2_SC7A20H_REG_CTRL4, (uint8_t)(0x80u | ((uint8_t)device->config.range << 4))},
        {H2_SC7A20H_REG_CTRL5, 0x00u},
        {H2_SC7A20H_REG_CTRL6, 0x00u},
        {H2_SC7A20H_REG_FIFO_CTRL, 0x00u},
        {H2_SC7A20H_REG_CTRL1,
         (uint8_t)(((uint8_t)device->config.odr << 4) | H2_SC7A20H_XYZ_ENABLE)},
    };
    device->powerdown_required = 1;
    for (size_t i = 0u; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        rc = write_reg(device, settings[i][0], settings[i][1]);
        if (rc != H2_PAL_OK) {
            break;
        }
    }
    if (rc == H2_PAL_OK) {
        rc = device->config.transport.sleep_ms(device->config.transport.user, 1u);
    }
    if (rc != H2_PAL_OK) {
        (void)h2_sc7a20h_close(device);
        return rc;
    }
    device->opened = 1;
    return H2_PAL_OK;
}

static int32_t axis_mg(const uint8_t *bytes, h2_sc7a20h_range_t range) {
    const uint16_t wire = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    int32_t count = (int32_t)(wire >> 4);
    if ((count & 0x800) != 0) {
        count -= 4096;
    }
    /* Manual section 13.13: 0x4000 equals 1g/2g/4g/8g by range. */
    const int32_t scaled = count * (int32_t)(2000u << (uint32_t)range);
    return (scaled + (scaled < 0 ? -1024 : 1024)) / 2048;
}

h2_pal_result_t h2_sc7a20h_read_sample(h2_sc7a20h_t *device, h2_sc7a20h_sample_t *out_sample) {
    if (out_sample == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_sample, 0, sizeof(*out_sample));
    if (device == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!device->opened) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint8_t status = 0u;
    h2_pal_result_t rc = read_reg(device, H2_SC7A20H_REG_STATUS, &status);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((status & 0x08u) == 0u) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    uint8_t bytes[6] = {0};
    rc = device->config.transport.read_regs(device->config.transport.user, H2_SC7A20H_REG_XYZ_BURST,
                                            bytes, sizeof(bytes));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < 3u; ++i) {
        out_sample->accel_mg[i] = axis_mg(&bytes[i * 2u], device->config.range);
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_sc7a20h_close(h2_sc7a20h_t *device) {
    if (device == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!device->powerdown_required) {
        return H2_PAL_OK;
    }
    device->opened = 0;
    h2_pal_result_t rc = write_reg(device, H2_SC7A20H_REG_CTRL1, H2_SC7A20H_XYZ_ENABLE);
    if (rc == H2_PAL_OK) {
        device->opened = 0;
        device->powerdown_required = 0;
    }
    return rc;
}

h2_pal_result_t h2_sc7a20h_destroy(h2_sc7a20h_t *device) {
    if (device == NULL) {
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = h2_sc7a20h_close(device);
    if (rc == H2_PAL_OK) {
        h2_pal_mem_free(device->config.mem, device);
    }
    return rc;
}
