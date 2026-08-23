#include "h2_linux_serial_host.h"
#include "h2_posix_serial_host_test.h"

int main(void) {
    return h2_posix_serial_host_run_tests(h2_linux_serial_host_api());
}
