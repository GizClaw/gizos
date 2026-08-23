#include "h2_bm8563_fake.h"

#include <string.h>

static h2_pal_result_t
fake_record_operation(h2_bm8563_fake_t *fake,
                      h2_bm8563_fake_operation_kind_t kind,
                      uint8_t start_register,
                      size_t data_length) {
    size_t operation_number;

    if (fake->operation_count >= H2_BM8563_FAKE_OPERATION_COUNT_MAX) {
        return H2_PAL_ERR_NO_SPACE;
    }
    fake->operations[fake->operation_count] = (h2_bm8563_fake_operation_t){
        .kind = kind,
        .start_register = start_register,
        .data_length = data_length,
    };
    ++fake->operation_count;
    operation_number = fake->operation_count;
    if (fake->fail_operation == operation_number) {
        fake->fail_operation = 0u;
        return fake->fail_result;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t fake_read_registers(void *user,
                                           uint8_t start_register,
                                           uint8_t *out_data,
                                           size_t data_length) {
    h2_bm8563_fake_t *fake = (h2_bm8563_fake_t *)user;
    h2_pal_result_t rc;

    if (fake == NULL || out_data == NULL || data_length == 0u ||
        start_register >= H2_BM8563_FAKE_REGISTER_COUNT ||
        data_length > H2_BM8563_FAKE_REGISTER_COUNT - start_register) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = fake_record_operation(
        fake, H2_BM8563_FAKE_OPERATION_READ, start_register, data_length);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memcpy(out_data, &fake->registers[start_register], data_length);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_write_registers(void *user,
                                            uint8_t start_register,
                                            const uint8_t *data,
                                            size_t data_length) {
    h2_bm8563_fake_t *fake = (h2_bm8563_fake_t *)user;
    h2_pal_result_t rc;

    if (fake == NULL || data == NULL || data_length == 0u ||
        start_register >= H2_BM8563_FAKE_REGISTER_COUNT ||
        data_length > H2_BM8563_FAKE_REGISTER_COUNT - start_register) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = fake_record_operation(
        fake, H2_BM8563_FAKE_OPERATION_WRITE, start_register, data_length);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memcpy(&fake->registers[start_register], data, data_length);
    return H2_PAL_OK;
}

void h2_bm8563_fake_init(h2_bm8563_fake_t *fake) {
    if (fake != NULL) {
        memset(fake, 0, sizeof(*fake));
    }
}

h2_bm8563_transport_t h2_bm8563_fake_transport(h2_bm8563_fake_t *fake) {
    static const h2_bm8563_transport_vtable_t vtable = {
        .read_registers = fake_read_registers,
        .write_registers = fake_write_registers,
    };

    return (h2_bm8563_transport_t){
        .user = fake,
        .vtable = &vtable,
    };
}

void h2_bm8563_fake_fail_operation(h2_bm8563_fake_t *fake,
                                   size_t operation,
                                   h2_pal_result_t result) {
    if (fake != NULL) {
        fake->fail_operation = operation;
        fake->fail_result = result;
    }
}
