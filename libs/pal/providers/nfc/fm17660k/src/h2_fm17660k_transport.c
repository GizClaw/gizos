#include "h2_fm17660k_transport.h"

#include <string.h>

#define H2_FM17660K_BYTE_READ_FLAG 0x80u

static const h2_fm17660k_byte_transport_config_t *byte_config(void *user) {
    return (const h2_fm17660k_byte_transport_config_t *)user;
}

static h2_pal_result_t byte_reset(void *user) {
    const h2_fm17660k_byte_transport_config_t *config = byte_config(user);
    return config->reset(config->reset_user);
}

static h2_pal_result_t byte_write_reg(
    void *user, uint8_t reg, uint8_t value) {
    const h2_fm17660k_byte_transport_config_t *config = byte_config(user);
    const uint8_t command[2] = {(uint8_t)(reg & 0x7fu), value};
    uint8_t echo = 0u;
    h2_pal_result_t result = config->endpoint.write_exact(
        config->endpoint.user, command, sizeof(command),
        config->operation_timeout_ms);
    if (result == H2_PAL_OK) {
        result = config->endpoint.read_exact(
            config->endpoint.user, &echo, sizeof(echo),
            config->operation_timeout_ms);
    }
    if (result == H2_PAL_OK && echo != command[0]) {
        return H2_PAL_ERR_FORMAT;
    }
    return result;
}

static h2_pal_result_t byte_write_regs(
    void *user, uint8_t reg, const uint8_t *data, size_t len) {
    if (data == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t index = 0u; index < len; ++index) {
        h2_pal_result_t result = byte_write_reg(user, reg, data[index]);
        if (result != H2_PAL_OK) {
            return result;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t byte_read_reg(
    void *user, uint8_t reg, uint8_t *out_value) {
    const h2_fm17660k_byte_transport_config_t *config = byte_config(user);
    const uint8_t command =
        (uint8_t)(H2_FM17660K_BYTE_READ_FLAG | (reg & 0x7fu));
    h2_pal_result_t result;
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = 0u;
    result = config->endpoint.write_exact(
        config->endpoint.user, &command, sizeof(command),
        config->operation_timeout_ms);
    if (result == H2_PAL_OK) {
        result = config->endpoint.read_exact(
            config->endpoint.user, out_value, sizeof(*out_value),
            config->operation_timeout_ms);
    }
    return result;
}

static h2_pal_result_t byte_read_regs(
    void *user, uint8_t reg, uint8_t *out_data, size_t len) {
    if (out_data == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t index = 0u; index < len; ++index) {
        h2_pal_result_t result = byte_read_reg(user, reg, &out_data[index]);
        if (result != H2_PAL_OK) {
            return result;
        }
    }
    return H2_PAL_OK;
}

static uint64_t byte_now_ms(void *user) {
    const h2_fm17660k_byte_transport_config_t *config = byte_config(user);
    return config->now_ms(config->clock_user);
}

static h2_pal_result_t byte_sleep_ms(void *user, uint32_t delay_ms) {
    const h2_fm17660k_byte_transport_config_t *config = byte_config(user);
    return config->sleep_ms(config->clock_user, delay_ms);
}

static const h2_fm17660k_transport_vtable_t s_byte_transport_vtable = {
    .reset = byte_reset,
    .write_reg = byte_write_reg,
    .write_regs = byte_write_regs,
    .read_reg = byte_read_reg,
    .read_regs = byte_read_regs,
    .now_ms = byte_now_ms,
    .sleep_ms = byte_sleep_ms,
};

h2_pal_result_t h2_fm17660k_transport_from_byte_endpoint(
    const h2_fm17660k_byte_transport_config_t *config,
    h2_fm17660k_transport_t *out_transport) {
    if (out_transport == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_transport, 0, sizeof(*out_transport));
    if (config == NULL || config->endpoint.write_exact == NULL ||
        config->endpoint.read_exact == NULL || config->reset == NULL ||
        config->now_ms == NULL || config->sleep_ms == NULL ||
        config->operation_timeout_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_transport->user = (void *)config;
    out_transport->vtable = &s_byte_transport_vtable;
    return H2_PAL_OK;
}
