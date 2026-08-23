#include "fake_flash.h"

#include <string.h>

static h2_pal_result_t fake_read(
    void *user,
    uint32_t address,
    void *data,
    size_t len)
{
    fake_flash_t *fake = (fake_flash_t *)user;
    if (fake->next_read_result != H2_PAL_OK) {
        h2_pal_result_t rc = fake->next_read_result;
        fake->next_read_result = H2_PAL_OK;
        return rc;
    }
    if (address > FAKE_FLASH_CAPACITY ||
        len > FAKE_FLASH_CAPACITY - address) {
        return H2_PAL_ERR_IO;
    }
    memcpy(data, &fake->bytes[address], len);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_erase(
    void *user,
    uint32_t address,
    size_t len)
{
    fake_flash_t *fake = (fake_flash_t *)user;
    if (fake->next_erase_result != H2_PAL_OK) {
        h2_pal_result_t rc = fake->next_erase_result;
        fake->next_erase_result = H2_PAL_OK;
        return rc;
    }
    if (address > FAKE_FLASH_CAPACITY ||
        len > FAKE_FLASH_CAPACITY - address) {
        return H2_PAL_ERR_IO;
    }
    memset(&fake->bytes[address], 0xff, len);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_write(
    void *user,
    uint32_t address,
    const void *data,
    size_t len)
{
    fake_flash_t *fake = (fake_flash_t *)user;
    size_t index;
    if (fake->next_write_result != H2_PAL_OK) {
        h2_pal_result_t rc = fake->next_write_result;
        fake->next_write_result = H2_PAL_OK;
        return rc;
    }
    if (address > FAKE_FLASH_CAPACITY ||
        len > FAKE_FLASH_CAPACITY - address) {
        return H2_PAL_ERR_IO;
    }
    for (index = 0u; index < len; ++index) {
        fake->bytes[address + index] &= ((const uint8_t *)data)[index];
    }
    return H2_PAL_OK;
}

void fake_flash_init(fake_flash_t *fake)
{
    memset(fake, 0, sizeof(*fake));
    memset(fake->bytes, 0xff, sizeof(fake->bytes));
}

const h2_bk3633_flash_driver_t *fake_flash_driver(fake_flash_t *fake)
{
    static h2_bk3633_flash_driver_t driver;
    driver = (h2_bk3633_flash_driver_t){
        .read = fake_read,
        .erase = fake_erase,
        .write = fake_write,
        .user = fake,
    };
    return &driver;
}
