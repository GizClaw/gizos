#include "h2/pal/hal/h2_pal_ble.h"

#include <assert.h>

typedef struct indication_fake {
    unsigned calls;
    uint32_t timeout_ms;
    h2_pal_result_t result;
} indication_fake_t;

static h2_pal_result_t indicate(
    void *user,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    indication_fake_t *fake = user;
    assert(conn_handle == 3u);
    assert(attr_handle == 5u);
    assert(data != NULL && len == 1u);
    ++fake->calls;
    fake->timeout_ms = timeout_ms;
    return fake->result;
}

int main(void) {
    indication_fake_t fake = {.result = H2_PAL_OK};
    const h2_pal_ble_vtable_t vtable = {.indicate = indicate};
    const h2_pal_ble_host_api_t api = {.user = &fake, .vtable = &vtable};
    const uint8_t byte = 0x5au;

    assert(h2_pal_ble_indicate(
               &api, 3u, 5u, &byte, sizeof(byte), 123u) == H2_PAL_OK);
    assert(fake.calls == 1u && fake.timeout_ms == 123u);
    assert(h2_pal_ble_indicate(
               NULL, 3u, 5u, &byte, sizeof(byte), 0u) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_ble_indicate(
               &api, 3u, 5u, NULL, sizeof(byte), 0u) ==
           H2_PAL_ERR_INVALID_ARG);
    fake.result = H2_PAL_ERR_TIMEOUT;
    assert(h2_pal_ble_indicate(
               &api, 3u, 5u, &byte, sizeof(byte), UINT32_MAX) ==
           H2_PAL_ERR_TIMEOUT);
    assert(fake.calls == 2u && fake.timeout_ms == UINT32_MAX);
    return 0;
}
