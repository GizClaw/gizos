#include <sys/select.h>
#include "h2_alipay_sdk_socket_abi.h"

/* Compile against the actual GCC16/Picolibc and unchanged vendor header. */
int h2_alipay_sdk_socket_abi_probe(void) {
  fd_set native;
  alipay_iot_fd_set vendor = {0};
  FD_ZERO(&native);
  FD_SET(0, &native);
  h2_alipay_sdk_fd_bits(&vendor)[0] = 1u;
  return FD_ISSET(0, &native) && h2_alipay_sdk_fd_bits(&vendor)[0] == 1u;
}
