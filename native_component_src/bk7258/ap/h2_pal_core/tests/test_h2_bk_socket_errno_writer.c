extern int test_legacy_errno;
#define errno test_legacy_errno
#include "h2_bk_socket_errno_writer.h"

void test_socket_failure(int error) {
    set_errno(error);
}
