#ifndef H2_BK_NET_RESULT_H
#define H2_BK_NET_RESULT_H
#include "h2/pal/core/h2_pal_errors.h"
#include <errno.h>

/* UDP sends are atomic: temporary socket/pbuf pressure consumes no datagram.
 * Preserve the retry contract used by DTLS and SCTP rather than terminating
 * an otherwise live connection when lwIP cannot allocate its copy. */
static inline int h2_bk_net_udp_send_result(int sent, int error) {
    if (sent >= 0) return sent;
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR ||
                   error == ENOMEM || error == ENOBUFS
               ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO;
}
#endif
