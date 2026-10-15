#ifndef H2_BK_SOCKET_ERRNO_READER_H
#define H2_BK_SOCKET_ERRNO_READER_H

/* Consume SDK errno declarations before replacing the read binding.
 * Subsequent guarded includes cannot restore the vendor global expression. */
#include <errno.h>
#include <lwip/opt.h>
#include <lwip/errno.h>
#include "h2_bk_task_tls.h"

#undef errno
#define errno (*__wrap___errno())

#endif
