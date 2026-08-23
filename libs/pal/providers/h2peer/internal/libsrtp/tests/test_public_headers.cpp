#include "h2_libsrtp.h"

int main() {
  h2_libsrtp_session_t *session = nullptr;
  h2_libsrtp_session_destroy(&session);
  return 0;
}
