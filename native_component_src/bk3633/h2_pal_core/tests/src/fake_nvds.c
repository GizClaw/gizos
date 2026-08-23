#include "fake_nvds.h"

#include <string.h>

static h2_bk3633_nvds_status_t fake_get(
    void *user,
    uint8_t tag,
    uint8_t *in_out_len,
    uint8_t *data)
{
    fake_nvds_t *fake = (fake_nvds_t *)user;
    fake_nvds_tag_t *stored = &fake->tags[tag];
    if (fake->next_get_status != H2_BK3633_NVDS_STATUS_OK) {
        h2_bk3633_nvds_status_t status = fake->next_get_status;
        fake->next_get_status = H2_BK3633_NVDS_STATUS_OK;
        return status;
    }
    if (!stored->present) {
        return H2_BK3633_NVDS_STATUS_NOT_FOUND;
    }
    if (*in_out_len < stored->len) {
        *in_out_len = stored->len;
        return H2_BK3633_NVDS_STATUS_LENGTH;
    }
    *in_out_len = stored->len;
    if (stored->len != 0u) {
        memcpy(data, stored->data, stored->len);
    }
    return H2_BK3633_NVDS_STATUS_OK;
}

static h2_bk3633_nvds_status_t fake_put(
    void *user,
    uint8_t tag,
    uint8_t len,
    const uint8_t *data)
{
    fake_nvds_t *fake = (fake_nvds_t *)user;
    fake_nvds_tag_t *stored = &fake->tags[tag];
    if (fake->next_put_status != H2_BK3633_NVDS_STATUS_OK) {
        h2_bk3633_nvds_status_t status = fake->next_put_status;
        fake->next_put_status = H2_BK3633_NVDS_STATUS_OK;
        return status;
    }
    stored->len = len;
    stored->present = true;
    if (len != 0u) {
        memcpy(stored->data, data, len);
    }
    return H2_BK3633_NVDS_STATUS_OK;
}

static h2_bk3633_nvds_status_t fake_del(void *user, uint8_t tag)
{
    fake_nvds_t *fake = (fake_nvds_t *)user;
    if (fake->next_del_status != H2_BK3633_NVDS_STATUS_OK) {
        h2_bk3633_nvds_status_t status = fake->next_del_status;
        fake->next_del_status = H2_BK3633_NVDS_STATUS_OK;
        return status;
    }
    if (!fake->tags[tag].present) {
        return H2_BK3633_NVDS_STATUS_NOT_FOUND;
    }
    fake->tags[tag].present = false;
    return H2_BK3633_NVDS_STATUS_OK;
}

void fake_nvds_init(fake_nvds_t *fake)
{
    memset(fake, 0, sizeof(*fake));
}

const h2_bk3633_nvds_driver_t *fake_nvds_driver(fake_nvds_t *fake)
{
    static h2_bk3633_nvds_driver_t driver;
    driver = (h2_bk3633_nvds_driver_t){
        .get = fake_get,
        .put = fake_put,
        .del = fake_del,
        .user = fake,
    };
    return &driver;
}

void fake_nvds_set_raw(
    fake_nvds_t *fake,
    uint8_t tag,
    const uint8_t *data,
    size_t len)
{
    fake_nvds_tag_t *stored = &fake->tags[tag];
    stored->present = true;
    stored->len = (uint8_t)len;
    if (len != 0u) {
        memcpy(stored->data, data, len);
    }
}
