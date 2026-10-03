#ifndef H2_BK_SOCKET_ERRNO_WRITER_H
#define H2_BK_SOCKET_ERRNO_WRITER_H

#include "h2_bk_task_tls.h"

/* The vendor socket implementation owns a separate global errno variable.
 * Retain that ABI for opaque SDK consumers, but also publish the exact error
 * to the current task. Match lwIP's existing zero-error/no-clear semantics. */
#ifndef set_errno
#define set_errno(error) do { \
    const int h2_socket_error = (error); \
    if (h2_socket_error != 0) { \
        errno = h2_socket_error; \
        *__wrap___errno() = h2_socket_error; \
    } \
} while (0)
#endif

#endif
