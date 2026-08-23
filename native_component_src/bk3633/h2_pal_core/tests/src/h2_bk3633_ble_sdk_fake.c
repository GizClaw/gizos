#include "h2_bk3633_ble_sdk_fake.h"

#include <stdlib.h>
#include <string.h>

#define FAKE_MESSAGE_MAX 32u
#define FAKE_SERVICE_MAX 8u
#define FAKE_SERVICE_ATTRIBUTE_MAX 16u

typedef struct fake_allocation {
    uint16_t id;
    uint16_t dest;
    uint16_t src;
    size_t size;
    max_align_t alignment;
    uint8_t payload[];
} fake_allocation_t;

static fake_allocation_t *s_allocations[FAKE_MESSAGE_MAX];
static h2_bk3633_ble_sdk_fake_message_t s_messages[FAKE_MESSAGE_MAX];
static size_t s_message_count;
static h2_bk3633_ble_sdk_fake_service_t s_services[FAKE_SERVICE_MAX];
static size_t s_service_count;
static size_t s_service_create_calls;
static size_t s_fail_service_create;
static size_t s_service_visibility_calls;
static size_t s_fail_service_visibility;
static uint16_t s_next_handle = 1u;
static size_t s_fail_next_allocations;

void h2_bk3633_ble_sdk_fake_reset(void) {
    for (size_t i = 0u; i < FAKE_MESSAGE_MAX; ++i) {
        free(s_allocations[i]);
        s_allocations[i] = NULL;
    }
    memset(s_messages, 0, sizeof(s_messages));
    memset(s_services, 0, sizeof(s_services));
    s_message_count = 0u;
    s_service_count = 0u;
    s_service_create_calls = 0u;
    s_fail_service_create = 0u;
    s_service_visibility_calls = 0u;
    s_fail_service_visibility = 0u;
    s_next_handle = 1u;
    s_fail_next_allocations = 0u;
}

void h2_bk3633_ble_sdk_fake_fail_next_allocations(size_t count) {
    s_fail_next_allocations = count;
}

void *h2_bk3633_ble_sdk_fake_alloc(
    size_t size, size_t extra, uint16_t id, uint16_t dest, uint16_t src) {
    if (s_fail_next_allocations != 0u) {
        --s_fail_next_allocations;
        return NULL;
    }
    if (size > SIZE_MAX - extra || s_message_count == FAKE_MESSAGE_MAX) {
        return NULL;
    }
    fake_allocation_t *allocation = calloc(1u, sizeof(*allocation) + size + extra);
    if (allocation == NULL) {
        return NULL;
    }
    allocation->id = id;
    allocation->dest = dest;
    allocation->src = src;
    allocation->size = size + extra;
    return allocation->payload;
}

void h2_bk3633_ble_sdk_fake_send(void *message) {
    if (message == NULL || s_message_count == FAKE_MESSAGE_MAX) {
        return;
    }
    fake_allocation_t *allocation =
        (fake_allocation_t *)((uint8_t *)message -
                              offsetof(fake_allocation_t, payload));
    s_allocations[s_message_count] = allocation;
    s_messages[s_message_count] = (h2_bk3633_ble_sdk_fake_message_t){
        .id = allocation->id,
        .dest = allocation->dest,
        .src = allocation->src,
        .payload = message,
        .payload_size = allocation->size,
    };
    ++s_message_count;
}

size_t h2_bk3633_ble_sdk_fake_message_count(void) {
    return s_message_count;
}

const h2_bk3633_ble_sdk_fake_message_t *
h2_bk3633_ble_sdk_fake_message(size_t index) {
    return index < s_message_count ? &s_messages[index] : NULL;
}

size_t h2_bk3633_ble_sdk_fake_service_count(void) {
    return s_service_count;
}

const h2_bk3633_ble_sdk_fake_service_t *
h2_bk3633_ble_sdk_fake_service(size_t index) {
    return index < s_service_count ? &s_services[index] : NULL;
}

void h2_bk3633_ble_sdk_fake_fail_service_create(size_t call_index) {
    s_fail_service_create = call_index;
}

void h2_bk3633_ble_sdk_fake_fail_service_visibility(size_t call_index) {
    s_fail_service_visibility = call_index;
}

static uint8_t fake_create_service(
    uint16_t *start_handle, const uint8_t *uuid, size_t uuid_len,
    uint8_t attribute_count, uint8_t permissions) {
    ++s_service_create_calls;
    if (s_fail_service_create == s_service_create_calls ||
        s_service_count == FAKE_SERVICE_MAX ||
        attribute_count > FAKE_SERVICE_ATTRIBUTE_MAX) {
        return ATT_ERR_INSUFF_RESOURCE;
    }
    h2_bk3633_ble_sdk_fake_service_t *service =
        &s_services[s_service_count++];
    service->handle = s_next_handle;
    service->uuid_len = uuid_len;
    memcpy(service->uuid, uuid, uuid_len);
    service->attribute_count = attribute_count;
    service->permissions = permissions;
    service->hidden =
        (permissions & (uint8_t)PERM(SVC_DIS, ENABLE)) != 0u;
    *start_handle = s_next_handle;
    s_next_handle = (uint16_t)(s_next_handle + attribute_count);
    return ATT_ERR_NO_ERROR;
}

uint8_t attm_svc_create_db(
    uint16_t *start_handle, uint16_t uuid, uint8_t *cfg_flag,
    uint8_t max_nb_att, uint8_t *att_tbl, uint16_t dest_id,
    const struct attm_desc *att_db, uint8_t sec_lvl) {
    (void)cfg_flag;
    (void)att_tbl;
    (void)dest_id;
    const uint8_t bytes[2] = {
        (uint8_t)(uuid & 0xffu),
        (uint8_t)(uuid >> 8),
    };
    uint8_t status =
        fake_create_service(
            start_handle, bytes, sizeof(bytes), max_nb_att, sec_lvl);
    if (status == ATT_ERR_NO_ERROR) {
        h2_bk3633_ble_sdk_fake_service_t *service =
            &s_services[s_service_count - 1u];
        for (size_t i = 0u; i < max_nb_att; ++i) {
            service->attribute_uuid_len[i] = 2u;
            service->attribute_ext_permissions[i] = att_db[i].ext_perm;
            service->attribute_uuid[i][0] =
                (uint8_t)(att_db[i].uuid & 0xffu);
            service->attribute_uuid[i][1] =
                (uint8_t)(att_db[i].uuid >> 8);
        }
    }
    return status;
}

uint8_t attm_svc_create_db_128(
    uint16_t *start_handle, const uint8_t uuid[16], uint8_t *cfg_flag,
    uint8_t max_nb_att, uint8_t *att_tbl, uint16_t dest_id,
    const struct attm_desc_128 *att_db, uint8_t sec_lvl) {
    (void)cfg_flag;
    (void)att_tbl;
    (void)dest_id;
    uint8_t status = fake_create_service(
        start_handle, uuid, 16u, max_nb_att, sec_lvl);
    if (status == ATT_ERR_NO_ERROR) {
        h2_bk3633_ble_sdk_fake_service_t *service =
            &s_services[s_service_count - 1u];
        for (size_t i = 0u; i < max_nb_att; ++i) {
            service->attribute_ext_permissions[i] = att_db[i].ext_perm;
            size_t uuid_len =
                (att_db[i].ext_perm & PERM(UUID_LEN, UUID_128)) != 0u
                    ? 16u
                    : 2u;
            service->attribute_uuid_len[i] = uuid_len;
            memcpy(service->attribute_uuid[i], att_db[i].uuid, uuid_len);
        }
    }
    return status;
}

uint8_t attmdb_svc_visibility_set(uint16_t handle, bool hide) {
    ++s_service_visibility_calls;
    if (s_fail_service_visibility == s_service_visibility_calls) {
        return ATT_ERR_REQUEST_NOT_SUPPORTED;
    }
    for (size_t i = 0u; i < s_service_count; ++i) {
        if (s_services[i].handle == handle) {
            s_services[i].hidden = hide;
            return ATT_ERR_NO_ERROR;
        }
    }
    return ATT_ERR_REQUEST_NOT_SUPPORTED;
}

uint8_t attm_att_set_value(
    uint16_t handle, att_size_t length, uint16_t offset, uint8_t *value) {
    (void)handle;
    (void)length;
    (void)offset;
    (void)value;
    return ATT_ERR_NO_ERROR;
}

void attm_convert_to128(
    uint8_t out[16], const uint8_t *uuid, uint8_t uuid_len) {
    static const uint8_t bluetooth_base[16] = {
        0xfbu, 0x34u, 0x9bu, 0x5fu, 0x80u, 0x00u, 0x00u, 0x80u,
        0x00u, 0x10u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    };
    if (uuid_len == 16u) {
        memcpy(out, uuid, 16u);
        return;
    }
    memcpy(out, bluetooth_base, sizeof(bluetooth_base));
    if (uuid_len == 2u) {
        out[12] = uuid[0];
        out[13] = uuid[1];
    }
}
