#include "h2_es8311_power.h"

#include <assert.h>
#include <stddef.h>

static const uint8_t expected[][2] = {
    {0x32, 0x00}, {0x17, 0x00}, {0x0e, 0xff}, {0x12, 0x02},
    {0x14, 0x00}, {0x0d, 0xfa}, {0x15, 0x00}, {0x02, 0x10},
    {0x00, 0x00}, {0x00, 0x1f}, {0x01, 0x30}, {0x01, 0x00},
    {0x45, 0x00}, {0x0d, 0xfc}, {0x02, 0x00},
};

typedef struct {
    size_t count;
    size_t fail_at;
} writer_t;

static int write_register(void *user, uint8_t reg, uint8_t value) {
    writer_t *writer = user;
    size_t index = writer->count++;
    assert(index < sizeof(expected) / sizeof(expected[0]));
    assert(reg == expected[index][0] && value == expected[index][1]);
    /* Distinct later errors must not replace the first failure. */
    return index >= writer->fail_at ? -(int)(index + 1u) : 0;
}

int main(void) {
    assert(h2_es8311_suspend(NULL, NULL) == -1);
    const size_t count = sizeof(expected) / sizeof(expected[0]);
    for (size_t fail_at = 0; fail_at <= count; ++fail_at) {
        writer_t writer = {0, fail_at};
        int expected_rc = fail_at == count ? 0 : -(int)(fail_at + 1u);
        assert(h2_es8311_suspend(&writer, write_register) == expected_rc);
        assert(writer.count == count);
        writer.count = 0;
        writer.fail_at = count;
        assert(h2_es8311_suspend(&writer, write_register) == 0);
        assert(writer.count == count);
    }
    return 0;
}
