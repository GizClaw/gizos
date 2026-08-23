#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "att.h"
#include "attm.h"

enum {
    TEST_HANDLE = 0x1234u,
    TEST_STRONG_STATUS = 0x5au,
};

static uint8_t s_permission;
static unsigned int s_get_calls;
static unsigned int s_set_calls;

uint8_t attm_svc_get_permission(uint16_t handle, uint8_t *permission)
{
    if (handle != TEST_HANDLE || permission == NULL)
        return 1u;
    ++s_get_calls;
    *permission = s_permission;
    return ATT_ERR_NO_ERROR;
}

uint8_t attm_svc_set_permission(uint16_t handle, uint8_t permission)
{
    if (handle != TEST_HANDLE)
        return 1u;
    ++s_set_calls;
    s_permission = permission;
    return ATT_ERR_NO_ERROR;
}

#ifdef H2_TEST_STRONG_OVERRIDE
uint8_t attmdb_svc_visibility_set(uint16_t handle, bool hide)
{
    return handle == TEST_HANDLE && hide ? TEST_STRONG_STATUS : 1u;
}
#endif

int main(void)
{
#ifdef H2_TEST_STRONG_OVERRIDE
    if (attmdb_svc_visibility_set(TEST_HANDLE, true) != TEST_STRONG_STATUS)
        return 1;
    if (s_get_calls != 0u || s_set_calls != 0u)
        return 2;
#else
    const uint8_t preserved =
        PERM(SVC_SECONDARY, ENABLE) | PERM(SVC_AUTH, ENABLE);
    s_permission = preserved;

    if (attmdb_svc_visibility_set(TEST_HANDLE, true) != ATT_ERR_NO_ERROR)
        return 1;
    if (s_permission != (uint8_t)(preserved | PERM(SVC_DIS, ENABLE)))
        return 2;

    if (attmdb_svc_visibility_set(TEST_HANDLE, false) != ATT_ERR_NO_ERROR)
        return 3;
    if (s_permission != preserved)
        return 4;
    if (s_get_calls != 2u || s_set_calls != 2u)
        return 5;
#endif
    return 0;
}
