#include "h2_darwin_platform.h"
#include "h2_posix_serial_host_internal.h"
#include "h2_posix_serial_host_test.h"

#include <IOKit/serial/ioss.h>
#include <assert.h>
#include <errno.h>
#include <string.h>

static int custom_speed_result;
static int custom_speed_errno;
static int custom_speed_fd;
static unsigned long custom_speed_request;
static speed_t custom_speed_value;
static int rollback_count;
static int rollback_fd;
static int rollback_action;
static struct termios rollback_attributes;

static int mock_set_custom_speed(
    int fd,
    unsigned long request,
    void *speed) {
    custom_speed_fd = fd;
    custom_speed_request = request;
    custom_speed_value = *(const speed_t *)speed;
    if (custom_speed_result != 0) {
        errno = custom_speed_errno;
    }
    return custom_speed_result;
}

static int mock_set_attributes(
    int fd,
    int action,
    const struct termios *attributes) {
    ++rollback_count;
    rollback_fd = fd;
    rollback_action = action;
    rollback_attributes = *attributes;
    return 0;
}

static void reset_custom_speed_fixture(void) {
    custom_speed_result = 0;
    custom_speed_errno = 0;
    custom_speed_fd = -1;
    custom_speed_request = 0u;
    custom_speed_value = 0;
    rollback_count = 0;
    rollback_fd = -1;
    rollback_action = -1;
    memset(&rollback_attributes, 0, sizeof(rollback_attributes));
}

static void test_custom_speed_and_rollback(void) {
    const h2_posix_serial_host_darwin_ops_t ops = {
        .set_attributes = mock_set_attributes,
        .set_custom_speed = mock_set_custom_speed,
    };
    struct termios original_attributes;
    const int fd = 17;
    memset(&original_attributes, 0, sizeof(original_attributes));
    original_attributes.c_cflag = CLOCAL | CREAD | CS8;

    reset_custom_speed_fixture();
    assert(h2_posix_serial_host_apply_darwin_custom_speed(
               fd,
               460800u,
               &original_attributes,
               &ops) == H2_PAL_OK);
    assert(custom_speed_fd == fd);
    assert(custom_speed_request == IOSSIOSPEED);
    assert(custom_speed_value == (speed_t)460800u);
    assert(rollback_count == 0);

    const int unsupported_errors[] = {EINVAL, ENOTTY};
    for (size_t index = 0u;
         index < sizeof(unsupported_errors) / sizeof(unsupported_errors[0]);
         ++index) {
        reset_custom_speed_fixture();
        custom_speed_result = -1;
        custom_speed_errno = unsupported_errors[index];
        assert(h2_posix_serial_host_apply_darwin_custom_speed(
                   fd,
                   460800u,
                   &original_attributes,
                   &ops) == H2_PAL_ERR_UNSUPPORTED);
        assert(rollback_count == 1);
        assert(rollback_fd == fd);
        assert(rollback_action == TCSANOW);
        assert(memcmp(
                   &rollback_attributes,
                   &original_attributes,
                   sizeof(original_attributes)) == 0);
    }
}

int main(void) {
    test_custom_speed_and_rollback();
    return h2_posix_serial_host_run_tests(h2_darwin_serial_host_api());
}
