#include "h2_libsrtp.h"

#if !defined(_WIN32)
#include <netinet/in.h>
#endif

int main(void) {
#if !defined(_WIN32)
    struct sockaddr_in address = {0};
    (void)address;
#endif
    h2_libsrtp_session_t *session = 0;
    h2_libsrtp_session_destroy(&session);
    return 0;
}
