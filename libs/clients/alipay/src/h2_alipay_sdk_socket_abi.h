#ifndef H2_ALIPAY_SDK_SOCKET_ABI_H
#define H2_ALIPAY_SDK_SOCKET_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Picolibc aliases fds_bits for its native fd_set. Keep the original vendor
 * field spelling and ABI isolated, then restore the system FD_* macros. */
#pragma push_macro("fds_bits")
#undef fds_bits
#include "alipay_net_kal.h"
_Static_assert(sizeof(alipay_iot_fd_set) == ALIPAY_IOT_MAX_IP_SOCKET_NUM,
               "unexpected vendor fd_set ABI");
_Static_assert(offsetof(alipay_iot_fd_set, fds_bits) == 0,
               "unexpected vendor fd_set layout");
static inline uint8_t* h2_alipay_sdk_fd_bits(alipay_iot_fd_set* set) {
  return set->fds_bits;
}
#pragma pop_macro("fds_bits")

#endif
