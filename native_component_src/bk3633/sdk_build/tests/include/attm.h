#ifndef COMPONENTS_BK3633_SDK_BUILD_TESTS_INCLUDE_ATTM_H_
#define COMPONENTS_BK3633_SDK_BUILD_TESTS_INCLUDE_ATTM_H_

#include <stdbool.h>
#include <stdint.h>

uint8_t attm_svc_get_permission(uint16_t handle, uint8_t *permission);
uint8_t attm_svc_set_permission(uint16_t handle, uint8_t permission);
uint8_t attmdb_svc_visibility_set(uint16_t handle, bool hide);

#endif
