#include "h2_bk_net_result.h"
#include <stddef.h>

int main(void) {
    /* An application must retain its complete DTLS flight through every
     * transient refusal, advancing only after the socket accepts all bytes. */
    const int pressure[] = {ENOMEM, ENOBUFS, EAGAIN, EWOULDBLOCK, EINTR};
    size_t advanced = 0u;
    for (size_t i = 0u; i < sizeof(pressure) / sizeof(pressure[0]); ++i) {
        int result = h2_bk_net_udp_send_result(-1, pressure[i]);
        if (result > 0) advanced += (size_t)result;
        if (result != H2_PAL_ERR_WOULD_BLOCK || advanced != 0u) return 1;
    }
    int sent = h2_bk_net_udp_send_result(638, ENOMEM);
    if (sent != 638) return 1;
    advanced += (size_t)sent;
    if (advanced != 638u || h2_bk_net_udp_send_result(0, EIO) != 0) return 1;
    if (h2_bk_net_udp_send_result(-1, EBADF) != H2_PAL_ERR_IO ||
        h2_bk_net_udp_send_result(-1, EINVAL) != H2_PAL_ERR_IO) return 1;
    return 0;
}
