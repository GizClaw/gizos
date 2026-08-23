#include "h2_libsrtp.h"

int main(void) {
    h2_libsrtp_session_t *session = 0;
    h2_libsrtp_session_destroy(&session);
    return 0;
}
