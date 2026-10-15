#include "h2_sc7a20h.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct sensor {
    uint8_t registers[128];
    uint8_t writes[32][2];
    size_t write_count;
    size_t read_count;
    uint8_t last_read;
    size_t last_read_len;
    uint64_t fail_writes;
    size_t fail_read;
    h2_pal_result_t sleep_error;
    uint32_t slept_ms;
    int fail_alloc;
    int allocations;
    int frees;
} sensor_t;

static void *allocate(void *user, size_t size) {
    sensor_t *sensor = user;
    if (sensor->fail_alloc)
        return NULL;
    sensor->allocations++;
    return malloc(size);
}

static void release(void *user, void *ptr) {
    ((sensor_t *)user)->frees++;
    free(ptr);
}

static h2_pal_result_t write_reg(void *user, uint8_t reg, uint8_t value) {
    sensor_t *sensor = user;
    assert(reg < 128u && !(reg >= 0x40u && reg <= 0x5fu));
    assert(sensor->write_count < 32u);
    const size_t index = sensor->write_count++;
    sensor->writes[index][0] = reg;
    sensor->writes[index][1] = value;
    if ((sensor->fail_writes & (UINT64_C(1) << index)) != 0u) {
        return H2_PAL_ERR_TIMEOUT;
    }
    sensor->registers[reg] = value;
    return H2_PAL_OK;
}

static h2_pal_result_t read_regs(void *user, uint8_t reg, uint8_t *out, size_t len) {
    sensor_t *sensor = user;
    sensor->read_count++;
    sensor->last_read = reg;
    sensor->last_read_len = len;
    if (sensor->read_count == sensor->fail_read)
        return -1000;
    assert((reg & 0x80u) != 0u || len == 1u);
    assert((size_t)(reg & 0x7fu) + len <= sizeof(sensor->registers));
    memcpy(out, &sensor->registers[reg & 0x7fu], len);
    return H2_PAL_OK;
}

static h2_pal_result_t sleep_ms(void *user, uint32_t ms) {
    sensor_t *sensor = user;
    sensor->slept_ms += ms;
    return sensor->sleep_error;
}

static h2_sc7a20h_config_t configure(sensor_t *sensor, h2_pal_mem_api_t *mem) {
    static const h2_pal_mem_vtable_t mem_vtable = {.alloc = allocate, .free = release};
    memset(sensor, 0, sizeof(*sensor));
    sensor->registers[0x0f] = 0x11u;
    sensor->registers[0x70] = 0x28u;
    *mem = (h2_pal_mem_api_t){.user = sensor, .vtable = &mem_vtable};
    return (h2_sc7a20h_config_t){
        .mem = mem,
        .transport = {sensor, write_reg, read_regs, sleep_ms},
        .range = H2_SC7A20H_RANGE_4G,
        .odr = H2_SC7A20H_ODR_50HZ,
        .data_ready_int1 = 1u,
    };
}

static void sample_words(sensor_t *sensor, uint16_t x, uint16_t y, uint16_t z) {
    const uint16_t words[] = {x, y, z};
    sensor->registers[0x27] = 0x08u;
    for (size_t i = 0u; i < 3u; ++i) {
        sensor->registers[0x28 + 2u * i] = (uint8_t)words[i];
        sensor->registers[0x29 + 2u * i] = (uint8_t)(words[i] >> 8);
    }
}

static void assert_zero(h2_sc7a20h_sample_t sample) {
    const h2_sc7a20h_sample_t zero = {0};
    assert(memcmp(&sample, &zero, sizeof(sample)) == 0);
}

static void test_configuration_and_lifecycle(void) {
    sensor_t sensor;
    h2_pal_mem_api_t mem;
    h2_sc7a20h_config_t config = configure(&sensor, &mem);
    h2_sc7a20h_t *device = NULL;
    assert(h2_sc7a20h_create(&config, &device) == H2_PAL_OK);
    assert(sensor.read_count == 0u && sensor.write_count == 0u);
    h2_sc7a20h_sample_t sample = {{1, 2, 3}};
    assert(h2_sc7a20h_read_sample(device, &sample) == H2_PAL_ERR_INVALID_STATE);
    assert_zero(sample);
    assert(h2_sc7a20h_open(device) == H2_PAL_OK);
    assert(sensor.registers[0x1f] == 0x01u);
    assert(sensor.registers[0x20] == 0x47u);
    assert(sensor.registers[0x21] == 0x00u);
    assert(sensor.registers[0x22] == 0x10u);
    assert(sensor.registers[0x23] == 0x90u);
    assert(sensor.registers[0x24] == 0x00u && sensor.registers[0x2e] == 0x00u);
    assert(sensor.slept_ms == 1u);
    const size_t writes = sensor.write_count;
    assert(h2_sc7a20h_open(device) == H2_PAL_OK);
    assert(sensor.write_count == writes);
    assert(h2_sc7a20h_close(device) == H2_PAL_OK);
    assert(sensor.registers[0x20] == 0x07u);
    assert(h2_sc7a20h_close(device) == H2_PAL_OK);
    assert(sensor.write_count == writes + 1u);
    assert(h2_sc7a20h_open(device) == H2_PAL_OK);
    assert(h2_sc7a20h_destroy(device) == H2_PAL_OK);
    assert(sensor.allocations == sensor.frees);
}

static void test_datasheet_conversion_and_burst(void) {
    for (uint32_t range = 0u; range < 4u; ++range) {
        sensor_t sensor;
        h2_pal_mem_api_t mem;
        h2_sc7a20h_config_t config = configure(&sensor, &mem);
        config.range = (h2_sc7a20h_range_t)range;
        config.data_ready_int1 = 0;
        h2_sc7a20h_t *device = NULL;
        assert(h2_sc7a20h_create(&config, &device) == H2_PAL_OK);
        assert(h2_sc7a20h_open(device) == H2_PAL_OK);
        assert(sensor.registers[0x22] == 0u);
        sample_words(&sensor, 0x4000u, 0xe000u, 0x8000u);
        h2_sc7a20h_sample_t sample;
        assert(h2_sc7a20h_read_sample(device, &sample) == H2_PAL_OK);
        assert(sensor.last_read == 0xa8u && sensor.last_read_len == 6u);
        assert(sample.accel_mg[0] == (int32_t)(1000u << range));
        assert(sample.accel_mg[1] == -(int32_t)(500u << range));
        assert(sample.accel_mg[2] == -(int32_t)(2000u << range));
        sample_words(&sensor, 0x001fu, 0xfff0u, 0x7ff0u);
        assert(h2_sc7a20h_read_sample(device, &sample) == H2_PAL_OK);
        const int32_t unit[] = {1, 2, 4, 8};
        const int32_t maximum[] = {1999, 3998, 7996, 15992};
        assert(sample.accel_mg[0] == unit[range]);
        assert(sample.accel_mg[1] == -unit[range]);
        assert(sample.accel_mg[2] == maximum[range]);
        assert(h2_sc7a20h_destroy(device) == H2_PAL_OK);
    }
}

static void test_identity_and_read_failures(void) {
    for (size_t identity = 0u; identity < 2u; ++identity) {
        sensor_t sensor;
        h2_pal_mem_api_t mem;
        h2_sc7a20h_config_t config = configure(&sensor, &mem);
        sensor.registers[identity == 0u ? 0x0f : 0x70] = 0;
        h2_sc7a20h_t *device = NULL;
        assert(h2_sc7a20h_create(&config, &device) == H2_PAL_OK);
        assert(h2_sc7a20h_open(device) == H2_PAL_ERR_NOT_FOUND);
        assert(sensor.write_count == 0u);
        assert(h2_sc7a20h_destroy(device) == H2_PAL_OK);
    }
    sensor_t sensor;
    h2_pal_mem_api_t mem;
    h2_sc7a20h_config_t config = configure(&sensor, &mem);
    h2_sc7a20h_t *device = NULL;
    assert(h2_sc7a20h_create(&config, &device) == H2_PAL_OK);
    sensor.fail_read = 1u;
    assert(h2_sc7a20h_open(device) == -1000);
    sensor.fail_read = 0u;
    assert(h2_sc7a20h_open(device) == H2_PAL_OK);
    h2_sc7a20h_sample_t sample = {{1, 2, 3}};
    assert(h2_sc7a20h_read_sample(device, &sample) == H2_PAL_ERR_WOULD_BLOCK);
    assert_zero(sample);
    sample_words(&sensor, 0x4000u, 0xe000u, 0x8000u);
    for (size_t offset = 1u; offset <= 2u; ++offset) {
        sample = (h2_sc7a20h_sample_t){{1, 2, 3}};
        sensor.fail_read = sensor.read_count + offset;
        assert(h2_sc7a20h_read_sample(device, &sample) == -1000);
        assert_zero(sample);
    }
    assert(h2_sc7a20h_destroy(device) == H2_PAL_OK);
}

static void test_configuration_failures_and_cleanup_retry(void) {
    for (size_t failure = 0u; failure < 10u; ++failure) {
        sensor_t sensor;
        h2_pal_mem_api_t mem;
        h2_sc7a20h_config_t config = configure(&sensor, &mem);
        if (failure < 9u)
            sensor.fail_writes = UINT64_C(1) << failure;
        else
            sensor.sleep_error = H2_PAL_ERR_TIMEOUT;
        h2_sc7a20h_t *device = NULL;
        assert(h2_sc7a20h_create(&config, &device) == H2_PAL_OK);
        assert(h2_sc7a20h_open(device) == H2_PAL_ERR_TIMEOUT);
        assert(sensor.registers[0x20] == 0x07u);
        assert(h2_sc7a20h_destroy(device) == H2_PAL_OK);
        assert(sensor.allocations == sensor.frees);
    }
    sensor_t sensor;
    h2_pal_mem_api_t mem;
    h2_sc7a20h_config_t config = configure(&sensor, &mem);
    h2_sc7a20h_t *device = NULL;
    assert(h2_sc7a20h_create(&config, &device) == H2_PAL_OK);
    sensor.fail_writes = (UINT64_C(1) << 3) | (UINT64_C(1) << 4) | (UINT64_C(1) << 5);
    assert(h2_sc7a20h_open(device) == H2_PAL_ERR_TIMEOUT);
    assert(h2_sc7a20h_destroy(device) == H2_PAL_ERR_TIMEOUT);
    assert(sensor.frees == 0);
    sensor.fail_writes = 0u;
    assert(h2_sc7a20h_destroy(device) == H2_PAL_OK);
    assert(sensor.frees == 1);
}

static void test_validation_and_allocation(void) {
    sensor_t sensor;
    h2_pal_mem_api_t mem;
    h2_sc7a20h_config_t config = configure(&sensor, &mem);
    h2_sc7a20h_t *device = (h2_sc7a20h_t *)(uintptr_t)1;
    assert(h2_sc7a20h_create(NULL, &device) == H2_PAL_ERR_INVALID_ARG);
    assert(device == NULL);
    assert(h2_sc7a20h_create(&config, NULL) == H2_PAL_ERR_INVALID_ARG);
    config.range = (h2_sc7a20h_range_t)-1;
    assert(h2_sc7a20h_create(&config, &device) == H2_PAL_ERR_INVALID_ARG);
    config.range = H2_SC7A20H_RANGE_2G;
    config.odr = (h2_sc7a20h_odr_t)0;
    assert(h2_sc7a20h_create(&config, &device) == H2_PAL_ERR_INVALID_ARG);
    config.odr = H2_SC7A20H_ODR_50HZ;
    config.transport.sleep_ms = NULL;
    assert(h2_sc7a20h_create(&config, &device) == H2_PAL_ERR_INVALID_ARG);
    config.transport.sleep_ms = sleep_ms;
    sensor.fail_alloc = 1;
    assert(h2_sc7a20h_create(&config, &device) == H2_PAL_ERR_NO_MEMORY);
    assert(device == NULL);
    assert(h2_sc7a20h_open(NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_sc7a20h_close(NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_sc7a20h_destroy(NULL) == H2_PAL_OK);
    h2_sc7a20h_sample_t sample = {{1, 2, 3}};
    assert(h2_sc7a20h_read_sample(NULL, &sample) == H2_PAL_ERR_INVALID_ARG);
    assert_zero(sample);
}

static void test_failed_close_invalidates_sampling(void) {
    sensor_t sensor;
    h2_pal_mem_api_t mem;
    h2_sc7a20h_config_t config = configure(&sensor, &mem);
    h2_sc7a20h_t *device = NULL;
    assert(h2_sc7a20h_create(&config, &device) == H2_PAL_OK);
    assert(h2_sc7a20h_open(device) == H2_PAL_OK);
    sensor.fail_writes = UINT64_C(1) << sensor.write_count;
    assert(h2_sc7a20h_close(device) == H2_PAL_ERR_TIMEOUT);
    h2_sc7a20h_sample_t sample = {{1, 2, 3}};
    assert(h2_sc7a20h_read_sample(device, &sample) == H2_PAL_ERR_INVALID_STATE);
    assert_zero(sample);
    sensor.fail_writes = 0u;
    assert(h2_sc7a20h_destroy(device) == H2_PAL_OK);
    assert(sensor.frees == sensor.allocations);
}

int main(void) {
    test_configuration_and_lifecycle();
    test_datasheet_conversion_and_burst();
    test_identity_and_read_failures();
    test_configuration_failures_and_cleanup_retry();
    test_validation_and_allocation();
    test_failed_close_invalidates_sampling();
    return 0;
}
