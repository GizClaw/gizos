#include "h2_alipay_sdk_socket_abi.h"
#include <string.h>

void alipay_iot_fd_zero(alipay_iot_fd_set* set) {
  if (set != NULL) memset(set, 0, sizeof(*set));
}
void alipay_iot_fd_setbit(int fd, alipay_iot_fd_set* set) {
  if (set != NULL && fd == 0) h2_alipay_sdk_fd_bits(set)[0] |= 1u;
}
int alipay_iot_fd_isset(int fd, alipay_iot_fd_set* set) {
  return set != NULL && fd == 0 && (h2_alipay_sdk_fd_bits(set)[0] & 2u) != 0u;
}
